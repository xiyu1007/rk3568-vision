// ============================================================================
// camera_source.cpp — 视频采集实现（V4L2 dmabuf 零拷贝 / mp4 解码）
// ============================================================================

#include "vision/camera_source.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <linux/videodev2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "vision/debug.hpp"
#include "vision/logger.hpp"

namespace vision {

namespace {

// V4L2 poll 超时（毫秒），使采集线程能周期性检查退出标志。
constexpr int kPollTimeoutMs = 500;

// 执行 V4L2 ioctl，自动重试被信号中断（EINTR）的调用。
int Xioctl(int file_descriptor, unsigned long request, void* argument) {
    int result;
    do {
        result = ::ioctl(file_descriptor, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
CameraSource::~CameraSource() {
    Stop();
}

// ---------------------------------------------------------------------------
// Initialize：按配置分发到 V4L2 / mp4
// ---------------------------------------------------------------------------
bool CameraSource::Initialize(const CaptureConfig& config) {
    config_ = config;
    is_mp4_source_ = (config_.source == "mp4");
    return is_mp4_source_ ? InitializeMp4() : InitializeV4l2();
}

// ---------------------------------------------------------------------------
// Start：启动采集线程
// ---------------------------------------------------------------------------
bool CameraSource::Start() {
    if (is_mp4_source_) {
        start_time_us_ = GetCurrentTimestampUs();
        frame_index_ = 0;
        running_.store(true);
        capture_thread_ = std::thread(&CameraSource::CaptureLoopMp4, this);
        return true;
    }

    // V4L2：STREAMON 开启数据流。
    int buffer_type = static_cast<int>(buffer_type_);
    if (Xioctl(video_fd_, VIDIOC_STREAMON, &buffer_type) < 0) {
        Logger::instance().error("v4l2: STREAMON failed: %s", strerror(errno));
        return false;
    }
    stream_started_ = true;
    running_.store(true);
    capture_thread_ = std::thread(&CameraSource::CaptureLoopV4l2, this);
    return true;
}

// ---------------------------------------------------------------------------
// Stop：停止采集线程并释放资源（幂等）
// ---------------------------------------------------------------------------
void CameraSource::Stop() {
    // 先停采集线程（若在运行）；资源释放始终执行（Initialize 失败时也要清理）。
    if (running_.exchange(false) && capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (is_mp4_source_) {
        // 释放 mp4 相关 FFmpeg 资源。
        if (sws_context_)     { sws_freeContext(sws_context_); sws_context_ = nullptr; }
        if (decoded_frame_)   { av_frame_free(&decoded_frame_); }
        if (nv12_frame_)      { av_frame_free(&nv12_frame_); }
        if (packet_)          { av_packet_free(&packet_); }
        if (codec_context_)   { avcodec_free_context(&codec_context_); }
        if (format_context_)  { avformat_close_input(&format_context_); }
    } else {
        // 释放 V4L2 相关资源：STREAMOFF、munmap、close fd、close dmabuf fd。
        if (stream_started_) {
            int buffer_type = static_cast<int>(buffer_type_);
            Xioctl(video_fd_, VIDIOC_STREAMOFF, &buffer_type);
            stream_started_ = false;
        }
        for (const MappedBuffer& buffer : mapped_buffers_) {
            if (buffer.address != nullptr && buffer.address != MAP_FAILED) {
                ::munmap(buffer.address, buffer.length);
            }
        }
        mapped_buffers_.clear();
        for (int fd : dma_fds_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        dma_fds_.clear();
        if (video_fd_ >= 0) {
            ::close(video_fd_);
            video_fd_ = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// RegisterFrameCallback：注册帧回调
// ---------------------------------------------------------------------------
void CameraSource::RegisterFrameCallback(std::function<void(FramePtr)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// InvokeCallback：回调分发（采集线程内调用）
// ---------------------------------------------------------------------------
void CameraSource::InvokeCallback(FramePtr frame) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (frame_callback_) {
        frame_callback_(std::move(frame));
    }
}

// ===========================================================================
//                          V4L2 采集实现
// ===========================================================================

// ---------------------------------------------------------------------------
// InitializeV4l2：打开设备并配置格式/缓冲/dmabuf
// ---------------------------------------------------------------------------
bool CameraSource::InitializeV4l2() {
    // 1. 打开设备（非阻塞，配合 poll）。
    video_fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (video_fd_ < 0) {
        Logger::instance().error("v4l2: open %s failed: %s",
                                 config_.device.c_str(), strerror(errno));
        return false;
    }

    // 2. 查询能力，选择 buffer 类型（rkisp 是 Multiplanar）。
    struct v4l2_capability capability{};
    if (Xioctl(video_fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        Logger::instance().error("v4l2: QUERYCAP failed");
        Stop();
        return false;
    }
    const uint32_t device_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                                     ? capability.device_caps
                                     : capability.capabilities;
    if (config_.use_multi_planar && (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (device_caps & V4L2_CAP_VIDEO_CAPTURE) {
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        Logger::instance().error("v4l2: no capture capability");
        Stop();
        return false;
    }

    // 3. 设置格式 NV12 + 分辨率。
    pixel_format_ = V4L2_PIX_FMT_NV12;
    struct v4l2_format format{};
    format.type = buffer_type_;
    if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        format.fmt.pix_mp.width       = config_.width;
        format.fmt.pix_mp.height      = config_.height;
        format.fmt.pix_mp.pixelformat = pixel_format_;
        format.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    } else {
        format.fmt.pix.width       = config_.width;
        format.fmt.pix.height      = config_.height;
        format.fmt.pix.pixelformat = pixel_format_;
        format.fmt.pix.field       = V4L2_FIELD_ANY;
    }
    if (Xioctl(video_fd_, VIDIOC_S_FMT, &format) < 0) {
        Logger::instance().error("v4l2: S_FMT failed: %s", strerror(errno));
        Stop();
        return false;
    }
    if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        width_     = format.fmt.pix_mp.width;
        height_    = format.fmt.pix_mp.height;
        plane_count_ = format.fmt.pix_mp.num_planes;
        stride_    = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        width_     = format.fmt.pix.width;
        height_    = format.fmt.pix.height;
        plane_count_ = 1;
        stride_    = format.fmt.pix.bytesperline;
    }

    // 4. 设置帧率。
    struct v4l2_streamparm parameter{};
    parameter.type = buffer_type_;
    if (Xioctl(video_fd_, VIDIOC_G_PARM, &parameter) == 0 &&
        (parameter.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parameter.parm.capture.timeperframe.numerator   = 1;
        parameter.parm.capture.timeperframe.denominator = config_.fps;
        if (Xioctl(video_fd_, VIDIOC_S_PARM, &parameter) == 0) {
            frame_rate_ = parameter.parm.capture.timeperframe.denominator;
        }
    }
    if (frame_rate_ == 0) {
        frame_rate_ = config_.fps;
    }

    // 5. 申请 + mmap + EXPBUF 导出 dmabuf fd。
    if (!RequestV4l2Buffers()) {
        Stop();
        return false;
    }

    Logger::instance().info("v4l2: %s %ux%u@%u stride=%u (planes=%u, zero-copy)",
                            config_.device.c_str(), width_, height_, frame_rate_,
                            stride_, plane_count_);
    return true;
}

// ---------------------------------------------------------------------------
// RequestV4l2Buffers：REQBUFS + QUERYBUF + mmap + EXPBUF
// ---------------------------------------------------------------------------
bool CameraSource::RequestV4l2Buffers() {
    struct v4l2_requestbuffers request{};
    request.count  = config_.buffer_count;
    request.type   = buffer_type_;
    request.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(video_fd_, VIDIOC_REQBUFS, &request) < 0) {
        Logger::instance().error("v4l2: REQBUFS failed: %s", strerror(errno));
        return false;
    }
    buffer_count_ = request.count;
    mapped_buffers_.resize(buffer_count_);
    dma_fds_.assign(buffer_count_, -1);   // -1 表示未导出，避免 close(0) 误关 stdin

    for (uint32_t index = 0; index < buffer_count_; ++index) {
        struct v4l2_buffer buffer{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buffer.type   = buffer_type_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = index;
        if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length   = VIDEO_MAX_PLANES;
        }
        if (Xioctl(video_fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            Logger::instance().error("v4l2: QUERYBUF[%u] failed", index);
            return false;
        }

        // rkisp 的 NV12 是单 plane（Y+UV 连续），取 plane 0 的 offset/length。
        const uint32_t plane_count =
            (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ? buffer.length : 1;
        const size_t length = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                                  ? planes[0].length
                                  : buffer.length;
        const off_t offset = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                                 ? planes[0].m.mem_offset
                                 : buffer.m.offset;

        void* address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, video_fd_, offset);
        if (address == MAP_FAILED) {
            Logger::instance().error("v4l2: mmap[%u] failed", index);
            return false;
        }
        mapped_buffers_[index] = {address, length};
        (void)plane_count;

        // 导出 dmabuf fd（零拷贝给 RGA 等下游）。
        struct v4l2_exportbuffer export_buffer{};
        export_buffer.type  = buffer_type_;
        export_buffer.index = index;
        export_buffer.plane = 0;
        if (Xioctl(video_fd_, VIDIOC_EXPBUF, &export_buffer) < 0) {
            Logger::instance().error("v4l2: EXPBUF[%u] failed: %s", index, strerror(errno));
            return false;
        }
        dma_fds_[index] = export_buffer.fd;
    }

    // 6. 所有 buffer 初始入队（QBUF），使后续 DQBUF 能取出。
    for (uint32_t index = 0; index < buffer_count_; ++index) {
        struct v4l2_buffer buffer{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buffer.type   = buffer_type_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = index;
        if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length   = plane_count_;
            for (uint32_t p = 0; p < plane_count_; ++p) {
                planes[p].length = mapped_buffers_[index].length;
            }
        }
        if (Xioctl(video_fd_, VIDIOC_QBUF, &buffer) < 0) {
            Logger::instance().error("v4l2: QBUF[%u] init failed: %s", index, strerror(errno));
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// CaptureLoopV4l2：采集线程（poll + DQBUF + 零拷贝回调）
// ---------------------------------------------------------------------------
void CameraSource::CaptureLoopV4l2() {
    while (running_.load()) {
        // 1. poll 限时等待 buffer 就绪。
        struct pollfd poll_fd{video_fd_, POLLIN, 0};
        if (::poll(&poll_fd, 1, kPollTimeoutMs) <= 0) {
            continue;
        }

        // 2. DQBUF 取出 buffer（不拷贝，直接引用 DMA buffer）。
        struct v4l2_buffer buffer{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buffer.type   = buffer_type_;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length   = VIDEO_MAX_PLANES;
        }
        if (Xioctl(video_fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno != EAGAIN) {
                Logger::instance().warn("v4l2: DQBUF failed: %s", strerror(errno));
            }
            continue;
        }

        // 3. 创建零拷贝帧：引用 mmap 地址 + fd，不 memcpy。
        const uint32_t buffer_index = buffer.index;
        const MappedBuffer& mapped = mapped_buffers_[buffer_index];

        FramePtr frame(new Frame, [this, buffer_index](Frame* raw_frame) {
            delete raw_frame;
            ReturnBuffer(buffer_index);   // 引用计数归零时 QBUF 归还
        });
        frame->sequence          = buffer.sequence;
        frame->width             = width_;
        frame->height            = height_;
        frame->capture_timestamp = GetCurrentTimestampUs();
        frame->nv12_data         = static_cast<const uint8_t*>(mapped.address);
        frame->nv12_stride       = stride_;
        frame->dma_fds.push_back(dma_fds_[buffer_index]);
        frame->buffer_index      = buffer_index;

        VISION_DEBUG_LOG("v4l2: frame seq=%llu buf=%u",
                         static_cast<unsigned long long>(frame->sequence),
                         buffer_index);

        // 4. 回调通知上层（回调里只入队）。
        InvokeCallback(std::move(frame));
    }
}

// ---------------------------------------------------------------------------
// ReturnBuffer：QBUF 归还 DMA buffer（由 Frame deleter 调用）
// ---------------------------------------------------------------------------
void CameraSource::ReturnBuffer(uint32_t buffer_index) {
    // 保护 QBUF ioctl（可能被下游线程并发调用）。
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (!running_.load() || !stream_started_) {
        return;   // 已停止，无需归还
    }

    struct v4l2_buffer buffer{};
    struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
    buffer.type   = buffer_type_;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index  = buffer_index;
    if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buffer.m.planes = planes;
        buffer.length   = plane_count_;
        for (uint32_t p = 0; p < plane_count_; ++p) {
            // 单 plane（rkisp）时 length 为整个 buffer 长度。
            planes[p].length = mapped_buffers_[buffer_index].length;
        }
    }
    if (Xioctl(video_fd_, VIDIOC_QBUF, &buffer) < 0) {
        Logger::instance().warn("v4l2: QBUF[%u] failed: %s", buffer_index, strerror(errno));
    }
}

// ===========================================================================
//                          mp4 解码实现
// ===========================================================================

// ---------------------------------------------------------------------------
// InitializeMp4：打开 mp4 文件并初始化解码器
// ---------------------------------------------------------------------------
bool CameraSource::InitializeMp4() {
    if (avformat_open_input(&format_context_, config_.file.c_str(), nullptr, nullptr) < 0) {
        Logger::instance().error("mp4: open %s failed", config_.file.c_str());
        return false;
    }
    avformat_find_stream_info(format_context_, nullptr);

    video_stream_index_ = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO,
                                              -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        Logger::instance().error("mp4: no video stream");
        Stop();
        return false;
    }
    AVStream* video_stream = format_context_->streams[video_stream_index_];

    codec_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
    codec_context_ = avcodec_alloc_context3(codec_);
    avcodec_parameters_to_context(codec_context_, video_stream->codecpar);
    if (avcodec_open2(codec_context_, codec_, nullptr) < 0) {
        Logger::instance().error("mp4: open decoder failed");
        Stop();
        return false;
    }

    width_  = codec_context_->width;
    height_ = codec_context_->height;
    const AVRational frame_rate = video_stream->avg_frame_rate;
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        frame_rate_ = static_cast<uint32_t>(av_q2d(frame_rate) + 0.5);
    }
    if (frame_rate_ == 0) {
        frame_rate_ = 25;
    }
    frame_interval_us_ = 1000000ULL / frame_rate_;

    decoded_frame_ = av_frame_alloc();
    nv12_frame_ = av_frame_alloc();
    nv12_frame_->format = AV_PIX_FMT_NV12;
    nv12_frame_->width  = width_;
    nv12_frame_->height = height_;
    av_frame_get_buffer(nv12_frame_, 32);
    sws_context_ = sws_getContext(width_, height_, codec_context_->pix_fmt,
                                  width_, height_, AV_PIX_FMT_NV12,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    packet_ = av_packet_alloc();

    Logger::instance().info("mp4: %s %ux%u@%u", config_.file.c_str(),
                            width_, height_, frame_rate_);
    return true;
}

// ---------------------------------------------------------------------------
// CaptureLoopMp4：解码循环（按源帧率节拍输出）
// ---------------------------------------------------------------------------
void CameraSource::CaptureLoopMp4() {
    while (running_.load()) {
        // 节拍：保持与源视频一致帧率。
        const uint64_t target_us = start_time_us_ + frame_index_ * frame_interval_us_;
        const uint64_t now_us = GetCurrentTimestampUs();
        if (now_us < target_us) {
            std::this_thread::sleep_for(std::chrono::microseconds(target_us - now_us));
        }

        int result = av_read_frame(format_context_, packet_);
        if (result < 0) {
            if (result == AVERROR_EOF) {
                // 循环播放。
                if (av_seek_frame(format_context_, video_stream_index_, 0,
                                  AVSEEK_FLAG_BACKWARD) < 0) {
                    return;
                }
                avcodec_flush_buffers(codec_context_);
                continue;
            }
            return;
        }
        if (packet_->stream_index != video_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        avcodec_send_packet(codec_context_, packet_);
        av_packet_unref(packet_);
        result = avcodec_receive_frame(codec_context_, decoded_frame_);
        if (result != 0) {
            continue;
        }

        // 转 NV12 到 CPU 内存（mp4 输入无 DMA，用 CPU 内存）。
        sws_scale(sws_context_, decoded_frame_->data, decoded_frame_->linesize,
                  0, height_, nv12_frame_->data, nv12_frame_->linesize);

        auto nv12_cpu = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(width_) * height_ * 3 / 2);
        std::memcpy(nv12_cpu->data(), nv12_frame_->data[0],
                    static_cast<size_t>(width_) * height_);
        std::memcpy(nv12_cpu->data() + static_cast<size_t>(width_) * height_,
                    nv12_frame_->data[1],
                    static_cast<size_t>(width_) * height_ / 2);

        FramePtr frame = std::make_shared<Frame>();
        frame->sequence          = frame_index_;
        frame->width             = width_;
        frame->height            = height_;
        frame->capture_timestamp = GetCurrentTimestampUs();
        frame->nv12_data         = nv12_cpu->data();
        frame->nv12_stride       = width_;
        frame->nv12_cpu          = std::move(nv12_cpu);

        ++frame_index_;
        InvokeCallback(std::move(frame));
    }
}

} // namespace vision
