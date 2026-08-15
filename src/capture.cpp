// ============================================================================
// capture.cpp — 视频采集实现（V4L2 摄像头 + mp4 文件）
// ============================================================================

#include "capture.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <linux/videodev2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "logger.hpp"

namespace vision {

namespace {
constexpr int kPollTimeoutMs = 500;   // V4L2 poll 超时（毫秒）
} // namespace

Capture::~Capture() {
    stop();
}

// ---------------------------------------------------------------------------
// open：按 source 分发到 v4l2 / mp4
// ---------------------------------------------------------------------------
bool Capture::open(const CaptureConfig& cfg) {
    cfg_    = cfg;
    is_mp4_ = (cfg_.source == "mp4");
    return is_mp4_ ? openMp4() : openV4l2();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
bool Capture::start() {
    if (is_mp4_) {
        start_us_  = nowUs();
        frame_idx_ = 0;
        return true;
    }
    int type = static_cast<int>(buf_type_);
    if (xioctl(VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("v4l2: STREAMON failed: %s", strerror(errno));
        return false;
    }
    started_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------
bool Capture::read(FramePtr& out) {
    return is_mp4_ ? readMp4(out) : readV4l2(out);
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
void Capture::stop() {
    if (is_mp4_) { stopMp4(); return; }
    stopV4l2();
}

// ===========================================================================
//                          V4L2 采集实现
// ===========================================================================

int Capture::xioctl(unsigned long request, void* arg) {
    int r;
    do {
        r = ::ioctl(fd_, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

bool Capture::openV4l2() {
    // 1. 打开设备（非阻塞，配合 poll）。
    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("v4l2: open %s failed: %s", cfg_.device.c_str(), strerror(errno));
        return false;
    }

    // 2. 查询能力，选缓冲区类型。
    struct v4l2_capability cap{};
    if (xioctl(VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("v4l2: QUERYCAP failed");
        stopV4l2(); return false;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    if (cfg_.use_mplane && (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        LOG_ERROR("v4l2: no capture capability");
        stopV4l2(); return false;
    }

    // 3. 设置格式 NV12 + 分辨率。
    pixfmt_ = V4L2_PIX_FMT_NV12;
    struct v4l2_format fmt{};
    fmt.type = buf_type_;
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fmt.fmt.pix_mp.width       = cfg_.width;
        fmt.fmt.pix_mp.height      = cfg_.height;
        fmt.fmt.pix_mp.pixelformat = pixfmt_;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    } else {
        fmt.fmt.pix.width       = cfg_.width;
        fmt.fmt.pix.height      = cfg_.height;
        fmt.fmt.pix.pixelformat = pixfmt_;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    }
    if (xioctl(VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("v4l2: S_FMT failed: %s", strerror(errno));
        stopV4l2(); return false;
    }
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        width_    = fmt.fmt.pix_mp.width;
        height_   = fmt.fmt.pix_mp.height;
        n_planes_ = fmt.fmt.pix_mp.num_planes;
        stride_   = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        width_    = fmt.fmt.pix.width;
        height_   = fmt.fmt.pix.height;
        n_planes_ = 1;
        stride_   = fmt.fmt.pix.bytesperline;
    }

    // 4. 设置帧率。
    struct v4l2_streamparm parm{};
    parm.type = buf_type_;
    if (xioctl(VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = cfg_.fps;
        if (xioctl(VIDIOC_S_PARM, &parm) == 0) {
            fps_ = parm.parm.capture.timeperframe.denominator;
        }
    }
    if (fps_ == 0) fps_ = cfg_.fps;

    // 5. 申请 + mmap + 入队。
    if (!requestBuffers() || !queueAll()) {
        stopV4l2(); return false;
    }
    LOG_INFO("v4l2: %s %ux%u@%u", cfg_.device.c_str(), width_, height_, fps_);
    return true;
}

bool Capture::requestBuffers() {
    struct v4l2_requestbuffers req{};
    req.count  = cfg_.buffer_count;
    req.type   = buf_type_;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("v4l2: REQBUFS failed");
        return false;
    }
    n_buffers_ = req.count;
    buffers_.resize(n_buffers_);

    for (uint32_t i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type = buf_type_; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES;
        }
        if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("v4l2: QUERYBUF[%u] failed", i);
            return false;
        }
        uint32_t cnt = (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                           ? buf.length : 1;
        buffers_[i].planes.resize(cnt);
        buffers_[i].lengths.resize(cnt);
        for (uint32_t p = 0; p < cnt; ++p) {
            size_t length = (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                                ? planes[p].length : buf.length;
            off_t offset = (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                               ? planes[p].m.mem_offset : buf.m.offset;
            void* addr = ::mmap(nullptr, length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_, offset);
            if (addr == MAP_FAILED) {
                LOG_ERROR("v4l2: mmap failed");
                return false;
            }
            buffers_[i].planes[p]  = addr;
            buffers_[i].lengths[p] = length;
        }
    }
    return true;
}

bool Capture::queueAll() {
    for (uint32_t i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type = buf_type_; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes; buf.length = n_planes_;
            for (uint32_t p = 0; p < n_planes_; ++p)
                planes[p].length = buffers_[i].lengths[p];
        }
        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("v4l2: QBUF[%u] failed", i);
            return false;
        }
    }
    return true;
}

bool Capture::readV4l2(FramePtr& out) {
    // 1. poll 限时等待。
    struct pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, kPollTimeoutMs) <= 0) return false;

    // 2. DQBUF 取出缓冲区。
    struct v4l2_buffer buf{};
    struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
    buf.type = buf_type_; buf.memory = V4L2_MEMORY_MMAP;
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES;
    }
    if (xioctl(VIDIOC_DQBUF, &buf) < 0) return false;

    // 3. 拷到 Frame（紧密排列 NV12，唯一必要拷贝）。
    auto frame = makeFrame(width_, height_);
    frame->capture_ts = nowUs();
    const auto& mb = buffers_[buf.index];

    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && n_planes_ >= 2) {
        const uint8_t* y  = static_cast<const uint8_t*>(mb.planes[0]);
        const uint8_t* uv = static_cast<const uint8_t*>(mb.planes[1]);
        uint8_t* dy  = frame->nv12.data();
        uint8_t* duv = frame->nv12.data() + static_cast<size_t>(width_) * height_;
        for (uint32_t r = 0; r < height_; ++r) {
            std::memcpy(dy, y, width_); dy += width_; y += stride_;
        }
        for (uint32_t r = 0; r < height_ / 2; ++r) {
            std::memcpy(duv, uv, width_); duv += width_; uv += stride_;
        }
    } else {
        const uint8_t* src = static_cast<const uint8_t*>(mb.planes[0]);
        uint8_t* dst = frame->nv12.data();
        size_t y_size = static_cast<size_t>(stride_) * height_;
        for (uint32_t r = 0; r < height_; ++r) {
            std::memcpy(dst, src, width_); dst += width_; src += stride_;
        }
        src = static_cast<const uint8_t*>(mb.planes[0]) + y_size;
        for (uint32_t r = 0; r < height_ / 2; ++r) {
            std::memcpy(dst, src, width_); dst += width_; src += stride_;
        }
    }

    // 4. QBUF 归还（复位 planes.length）。
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        for (uint32_t p = 0; p < n_planes_; ++p)
            planes[p].length = buffers_[buf.index].lengths[p];
    }
    xioctl(VIDIOC_QBUF, &buf);

    out = std::move(frame);
    return true;
}

void Capture::stopV4l2() {
    if (started_) {
        int type = static_cast<int>(buf_type_);
        xioctl(VIDIOC_STREAMOFF, &type);
        started_ = false;
    }
    for (auto& mb : buffers_)
        for (size_t p = 0; p < mb.planes.size(); ++p)
            if (mb.planes[p] && mb.planes[p] != MAP_FAILED)
                ::munmap(mb.planes[p], mb.lengths[p]);
    buffers_.clear();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

// ===========================================================================
//                          mp4 解码实现
// ===========================================================================

bool Capture::openMp4() {
    if (avformat_open_input(&fmt_ctx_, cfg_.file.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("mp4: open %s failed", cfg_.file.c_str());
        return false;
    }
    avformat_find_stream_info(fmt_ctx_, nullptr);

    video_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx_ < 0) { LOG_ERROR("mp4: no video stream"); stopMp4(); return false; }
    AVStream* vs = fmt_ctx_->streams[video_idx_];

    codec_ = avcodec_find_decoder(vs->codecpar->codec_id);
    codec_ctx_ = avcodec_alloc_context3(codec_);
    avcodec_parameters_to_context(codec_ctx_, vs->codecpar);
    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        LOG_ERROR("mp4: open decoder failed"); stopMp4(); return false;
    }

    width_  = codec_ctx_->width;
    height_ = codec_ctx_->height;
    AVRational fr = vs->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) fps_ = static_cast<uint32_t>(av_q2d(fr) + 0.5);
    if (fps_ == 0) fps_ = 25;
    frame_interval_us_ = 1000000ULL / fps_;

    dec_frame_  = av_frame_alloc();
    nv12_frame_ = av_frame_alloc();
    nv12_frame_->format = AV_PIX_FMT_NV12;
    nv12_frame_->width  = width_;
    nv12_frame_->height = height_;
    av_frame_get_buffer(nv12_frame_, 32);
    sws_ = sws_getContext(width_, height_, codec_ctx_->pix_fmt,
                          width_, height_, AV_PIX_FMT_NV12,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    pkt_ = av_packet_alloc();

    LOG_INFO("mp4: %s %ux%u@%u", cfg_.file.c_str(), width_, height_, fps_);
    return true;
}

bool Capture::readMp4(FramePtr& out) {
    // 节拍：保持与源视频一致帧率。
    uint64_t target = start_us_ + frame_idx_ * frame_interval_us_;
    uint64_t now = nowUs();
    if (now < target)
        std::this_thread::sleep_for(std::chrono::microseconds(target - now));

    while (true) {
        int ret = av_read_frame(fmt_ctx_, pkt_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {   // 循环播放
                if (av_seek_frame(fmt_ctx_, video_idx_, 0, AVSEEK_FLAG_BACKWARD) < 0)
                    return false;
                avcodec_flush_buffers(codec_ctx_);
                continue;
            }
            return false;
        }
        if (pkt_->stream_index != video_idx_) { av_packet_unref(pkt_); continue; }

        avcodec_send_packet(codec_ctx_, pkt_);
        av_packet_unref(pkt_);
        ret = avcodec_receive_frame(codec_ctx_, dec_frame_);
        if (ret == 0) {
            sws_scale(sws_, dec_frame_->data, dec_frame_->linesize, 0, height_,
                      nv12_frame_->data, nv12_frame_->linesize);
            auto f = makeFrame(width_, height_);
            std::memcpy(f->nv12.data(), nv12_frame_->data[0],
                        static_cast<size_t>(width_) * height_);
            std::memcpy(f->nv12.data() + static_cast<size_t>(width_) * height_,
                        nv12_frame_->data[1],
                        static_cast<size_t>(width_) * height_ / 2);
            f->capture_ts = nowUs();
            out = std::move(f);
            ++frame_idx_;
            return true;
        }
    }
}

void Capture::stopMp4() {
    if (sws_)         { sws_freeContext(sws_); sws_ = nullptr; }
    if (dec_frame_)   { av_frame_free(&dec_frame_); }
    if (nv12_frame_)  { av_frame_free(&nv12_frame_); }
    if (pkt_)         { av_packet_free(&pkt_); }
    if (codec_ctx_)   { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_)     { avformat_close_input(&fmt_ctx_); }
}

} // namespace vision
