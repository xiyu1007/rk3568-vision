// ============================================================================
// v4l2_capture.cpp — V4L2 采集实现
// ============================================================================

#include "v4l2_capture.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "logger.hpp"

namespace vision {

namespace {
// 单次 poll 等待时长（毫秒）：既保证采集线程能及时响应退出，又不至于频繁空转。
constexpr int kPollTimeoutMs = 500;
} // namespace

// ---------------------------------------------------------------------------
// 析构：确保资源释放
// ---------------------------------------------------------------------------
V4l2Capture::~V4l2Capture() {
    stop();
}

// ---------------------------------------------------------------------------
// ioctl 封装：被信号中断（EINTR）时自动重试
// ---------------------------------------------------------------------------
int V4l2Capture::xioctl(unsigned long request, void* arg) {
    int r;
    do {
        r = ::ioctl(fd_, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

// ---------------------------------------------------------------------------
// open：完整初始化
// ---------------------------------------------------------------------------
bool V4l2Capture::open(const CaptureConfig& cfg) {
    // 1. 打开设备（非阻塞，配合 poll 使用）。
    fd_ = ::open(cfg.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("v4l2: open %s failed: %s", cfg.device.c_str(), strerror(errno));
        return false;
    }

    // 2. 查询设备能力，选择缓冲区类型（MPLANE 优先）。
    struct v4l2_capability cap{};
    if (xioctl(VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("v4l2: QUERYCAP failed: %s", strerror(errno));
        stop(); return false;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    if (cfg.use_mplane && (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        buf_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        LOG_ERROR("v4l2: device does not support video capture");
        stop(); return false;
    }
    LOG_INFO("v4l2: capability 0x%08x, buffer type %s",
             caps, (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                       ? "MPLANE" : "SINGLE");

    // 3. 设置像素格式与分辨率。
    pixfmt_ = V4L2_PIX_FMT_NV12;   // IMX415 经 ISP 后输出 NV12
    struct v4l2_format fmt{};
    fmt.type = buf_type_;
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fmt.fmt.pix_mp.width       = cfg.width;
        fmt.fmt.pix_mp.height      = cfg.height;
        fmt.fmt.pix_mp.pixelformat = pixfmt_;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    } else {
        fmt.fmt.pix.width       = cfg.width;
        fmt.fmt.pix.height      = cfg.height;
        fmt.fmt.pix.pixelformat = pixfmt_;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    }
    if (xioctl(VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("v4l2: S_FMT failed: %s", strerror(errno));
        stop(); return false;
    }
    // 驱动可能修改分辨率/格式，回读实际值。
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        width_   = fmt.fmt.pix_mp.width;
        height_  = fmt.fmt.pix_mp.height;
        pixfmt_  = fmt.fmt.pix_mp.pixelformat;
        n_planes_ = fmt.fmt.pix_mp.num_planes;
        stride_  = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        width_   = fmt.fmt.pix.width;
        height_  = fmt.fmt.pix.height;
        pixfmt_  = fmt.fmt.pix.pixelformat;
        n_planes_ = 1;
        stride_  = fmt.fmt.pix.bytesperline;
    }
    if (pixfmt_ != V4L2_PIX_FMT_NV12) {
        LOG_WARN("v4l2: driver output not NV12 (0x%08x), downstream may fail", pixfmt_);
    }
    LOG_INFO("v4l2: format %ux%u NV12, %u planes, stride %u",
             width_, height_, n_planes_, stride_);

    // 4. 设置帧率。
    struct v4l2_streamparm parm{};
    parm.type = buf_type_;
    if (xioctl(VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = cfg.fps;
        if (xioctl(VIDIOC_S_PARM, &parm) == 0) {
            fps_ = parm.parm.capture.timeperframe.denominator;
        }
    }
    if (fps_ == 0) fps_ = cfg.fps;   // 驱动不支持时退化为配置值
    LOG_INFO("v4l2: fps = %u", fps_);

    // 5. 申请 + mmap 缓冲区。
    if (!requestBuffers()) { stop(); return false; }

    // 6. 全部入队。
    if (!queueAll()) { stop(); return false; }

    return true;
}

// ---------------------------------------------------------------------------
// requestBuffers：REQBUFS + QUERYBUF + mmap
// ---------------------------------------------------------------------------
bool V4l2Capture::requestBuffers() {
    struct v4l2_requestbuffers req{};
    req.count  = 6;                 // 6 个 DMA buffer（够 25fps 使用）
    req.type   = buf_type_;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("v4l2: REQBUFS failed: %s", strerror(errno));
        return false;
    }
    n_buffers_ = req.count;
    buffers_.resize(n_buffers_);
    LOG_INFO("v4l2: %u buffers requested", n_buffers_);

    // 逐个缓冲区查询 + mmap。
    for (uint32_t i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type   = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length   = VIDEO_MAX_PLANES;
        }
        if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("v4l2: QUERYBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }

        uint32_t cnt = (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                           ? buf.length : 1;
        if (cnt != n_planes_) {
            LOG_WARN("v4l2: plane count mismatch (%u vs %u)", cnt, n_planes_);
        }

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
                LOG_ERROR("v4l2: mmap[%u][%u] failed: %s", i, p, strerror(errno));
                return false;
            }
            buffers_[i].planes[p]  = addr;
            buffers_[i].lengths[p] = length;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// queueAll：所有缓冲区入队
// ---------------------------------------------------------------------------
bool V4l2Capture::queueAll() {
    for (uint32_t i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf{};
        struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
        buf.type   = buf_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length   = n_planes_;
            for (uint32_t p = 0; p < n_planes_; ++p) {
                planes[p].length = buffers_[i].lengths[p];
            }
        }
        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("v4l2: QBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// start：启动流
// ---------------------------------------------------------------------------
bool V4l2Capture::start() {
    int type = static_cast<int>(buf_type_);
    if (xioctl(VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("v4l2: STREAMON failed: %s", strerror(errno));
        return false;
    }
    started_ = true;
    LOG_INFO("v4l2: stream started");
    return true;
}

// ---------------------------------------------------------------------------
// read：读取一帧（poll + DQBUF + 拷贝 + QBUF）
// ---------------------------------------------------------------------------
bool V4l2Capture::read(FramePtr& out) {
    // 1. poll 限时等待数据可读（也便于响应退出信号）。
    struct pollfd pfd{fd_, POLLIN, 0};
    int ret = ::poll(&pfd, 1, kPollTimeoutMs);
    if (ret <= 0) {
        return false;   // 超时或出错（无新帧）
    }

    // 2. DQBUF 取出一个填充好的缓冲区。
    struct v4l2_buffer buf{};
    struct v4l2_plane  planes[VIDEO_MAX_PLANES]{};
    buf.type   = buf_type_;
    buf.memory = V4L2_MEMORY_MMAP;
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length   = VIDEO_MAX_PLANES;
    }
    if (xioctl(VIDIOC_DQBUF, &buf) < 0) {
        return false;   // EAGAIN 等临时错误，直接返回
    }

    // 3. 拷贝 NV12 数据到 Frame（从 mmap 循环缓冲拷到自有内存，唯一必要拷贝）。
    auto frame = makeFrame(width_, height_, width_);
    frame->capture_ts = nowUs();

    const auto& mb = buffers_[buf.index];
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && n_planes_ >= 2) {
        // 平面0 = Y（height 行），平面1 = UV（height/2 行）。
        // 逐行拷贝以消除 stride 与 width 的差异（得到紧密排列的 NV12）。
        const uint8_t* y  = static_cast<const uint8_t*>(mb.planes[0]);
        const uint8_t* uv = static_cast<const uint8_t*>(mb.planes[1]);
        uint8_t* dst_y  = frame->nv12.data();
        uint8_t* dst_uv = frame->nv12.data() + static_cast<size_t>(width_) * height_;
        for (uint32_t r = 0; r < height_; ++r) {
            std::memcpy(dst_y, y, width_);
            dst_y += width_;
            y     += stride_;
        }
        for (uint32_t r = 0; r < height_ / 2; ++r) {
            std::memcpy(dst_uv, uv, width_);
            dst_uv += width_;
            uv     += stride_;
        }
    } else {
        // 单平面 NV12：Y 与 UV 连续存放（stride 对齐）。
        const uint8_t* src = static_cast<const uint8_t*>(mb.planes[0]);
        uint8_t* dst = frame->nv12.data();
        size_t y_size  = static_cast<size_t>(stride_) * height_;
        size_t uv_size = static_cast<size_t>(stride_) * (height_ / 2);
        for (uint32_t r = 0; r < height_; ++r) {
            std::memcpy(dst, src, width_);
            dst += width_; src += stride_;
        }
        // UV 部分
        src = static_cast<const uint8_t*>(mb.planes[0]) + y_size;
        for (uint32_t r = 0; r < height_ / 2; ++r) {
            std::memcpy(dst, src, width_);
            dst += width_; src += stride_;
        }
        (void)uv_size;
    }

    // 4. QBUF 归还缓冲区（归还前复位 planes.length，DQBUF 可能已修改它）。
    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        for (uint32_t p = 0; p < n_planes_; ++p) {
            planes[p].length = buffers_[buf.index].lengths[p];
        }
    }
    xioctl(VIDIOC_QBUF, &buf);

    out = std::move(frame);
    return true;
}

// ---------------------------------------------------------------------------
// stop：停止并释放
// ---------------------------------------------------------------------------
void V4l2Capture::stop() {
    if (started_) {
        int type = static_cast<int>(buf_type_);
        xioctl(VIDIOC_STREAMOFF, &type);
        started_ = false;
    }
    // munmap 所有映射。
    for (auto& mb : buffers_) {
        for (size_t p = 0; p < mb.planes.size(); ++p) {
            if (mb.planes[p] && mb.planes[p] != MAP_FAILED) {
                ::munmap(mb.planes[p], mb.lengths[p]);
            }
        }
    }
    buffers_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    LOG_INFO("v4l2: stopped");
}

} // namespace vision
