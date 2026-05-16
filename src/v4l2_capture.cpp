#include "v4l2_capture.hpp"

#include <cstring>
#include <errno.h>
#include <sys/epoll.h>

namespace rk3568_vision {

V4L2Capture::V4L2Capture() = default;

V4L2Capture::~V4L2Capture() {
    stop();
}

int V4L2Capture::xioctl(int request, void* arg) {
    int r;
    do { r = ioctl(fd_, request, arg); }
    while (r == -1 && errno == EINTR);
    return r;
}

bool V4L2Capture::open(const std::string& device, uint32_t width, uint32_t height,
                       uint32_t fps, const std::string& pixfmt, uint32_t buf_count) {
    device_ = device;
    width_  = width;
    height_ = height;
    fps_    = fps;

    // 解析像素格式
    if (pixfmt == "NV12" || pixfmt == "nv12")
        pixel_format_ = v4l2_fourcc('N', 'V', '1', '2');
    else if (pixfmt == "MJPEG" || pixfmt == "mjpeg")
        pixel_format_ = v4l2_fourcc('M', 'J', 'P', 'G');
    else if (pixfmt == "YUYV" || pixfmt == "yuyv")
        pixel_format_ = v4l2_fourcc('Y', 'U', 'Y', 'V');
    else {
        LOG_ERROR("Unknown pixel format: %s", pixfmt.c_str());
        return false;
    }

    fd_ = ::open(device_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("Cannot open device %s: %s", device_.c_str(), strerror(errno));
        return false;
    }

    if (!init_device())  { close(fd_); fd_ = -1; return false; }
    if (!init_format())  { close(fd_); fd_ = -1; return false; }
    if (!init_fps())     { /* 非致命错误 */ }
    if (!init_mmap(buf_count)) { close(fd_); fd_ = -1; return false; }

    LOG_INFO("V4L2 device opened: %s %ux%u@%ufps %s",
             device_.c_str(), width_, height_, fps_, pixfmt.c_str());
    return true;
}

bool V4L2Capture::init_device() {
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (xioctl(VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        return false;
    }

    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? cap.device_caps : cap.capabilities;

    if (!(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        LOG_ERROR("Device does not support video capture");
        return false;
    }

    if (!(caps & V4L2_CAP_STREAMING)) {
        LOG_ERROR("Device does not support streaming I/O");
        return false;
    }

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    return true;
}

bool V4L2Capture::init_format() {
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = buffer_type_;
    fmt.fmt.pix_mp.width        = width_;
    fmt.fmt.pix_mp.height       = height_;
    fmt.fmt.pix_mp.pixelformat  = pixel_format_;
    fmt.fmt.pix_mp.field        = V4L2_FIELD_ANY;

    if (xioctl(VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_S_FMT failed: %s", strerror(errno));
        return false;
    }

    width_       = fmt.fmt.pix_mp.width;
    height_      = fmt.fmt.pix_mp.height;
    pixel_format_ = fmt.fmt.pix_mp.pixelformat;

    num_planes_ = fmt.fmt.pix_mp.num_planes;
    if (num_planes_ == 0) num_planes_ = 1;

    for (uint32_t i = 0; i < num_planes_; ++i) {
        strides_[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        sizes_[i]   = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    }

    LOG_INFO("Format set: %ux%u planes=%u", width_, height_, num_planes_);
    return true;
}

bool V4L2Capture::init_fps() {
    v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = buffer_type_;

    if (xioctl(VIDIOC_G_PARM, &parm) < 0) {
        if (errno == ENOTTY) return true;
        return false;
    }

    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME))
        return true;

    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps_;

    if (xioctl(VIDIOC_S_PARM, &parm) < 0) {
        LOG_WARN("VIDIOC_S_PARM failed, FPS may not be exact");
        return false;
    }

    fps_ = parm.parm.capture.timeperframe.denominator;
    LOG_INFO("FPS set to %u", fps_);
    return true;
}

bool V4L2Capture::init_mmap(uint32_t buf_count) {
    v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = buf_count;
    req.type   = buffer_type_;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }

    LOG_INFO("Requested %u buffers, got %u", buf_count, req.count);

    buffers_.reserve(req.count);

    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf;
        v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type   = buffer_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length   = VIDEO_MAX_PLANES;
        }

        if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QUERYBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }

        off_t offset = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                       ? planes[0].m.mem_offset : buf.m.offset;
        size_t length = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                        ? planes[0].length : buf.length;

        buffers_.emplace_back(std::make_unique<V4L2Buffer>(
            fd_, length, offset, i));

        if (!buffers_.back()->valid()) {
            LOG_ERROR("mmap failed for buffer %u", i);
            return false;
        }
    }

    return true;
}

bool V4L2Capture::queue_all_buffers() {
    for (size_t i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer buf;
        v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type   = buffer_type_;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length   = num_planes_;
            for (uint32_t p = 0; p < num_planes_; ++p) {
                planes[p].length = sizes_[p];
            }
        }

        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QBUF[%zu] failed: %s", i, strerror(errno));
            return false;
        }
    }
    return true;
}

bool V4L2Capture::start() {
    if (!queue_all_buffers()) return false;

    int type = buffer_type_;
    if (xioctl(VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("VIDIOC_STREAMON failed: %s", strerror(errno));
        return false;
    }

    // 创建 epoll，注册 V4L2 fd，等待 DQBUF 就绪（替代 busy-wait sleep）
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev);
    }

    running_ = true;
    LOG_INFO("V4L2 streaming started (epoll)");
    return true;
}

void V4L2Capture::stop() {
    if (!running_.exchange(false)) return;

    int type = buffer_type_;
    xioctl(VIDIOC_STREAMOFF, &type);

    if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }

    buffers_.clear();

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    LOG_INFO("V4L2 streaming stopped");
}

std::shared_ptr<FrameBuffer> V4L2Capture::capture() {
    if (!running_.load(std::memory_order_relaxed)) return nullptr;

    // epoll 等待帧就绪（100ms超时，用于周期性检查 running_ 状态）
    if (epoll_fd_ >= 0) {
        struct epoll_event ev;
        int n = epoll_wait(epoll_fd_, &ev, 1, 100);
        if (n <= 0) return nullptr;
    }

    v4l2_buffer buf;
    v4l2_plane  planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type   = buffer_type_;
    buf.memory = V4L2_MEMORY_MMAP;

    if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length   = VIDEO_MAX_PLANES;
    }

    if (xioctl(VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return nullptr;
        LOG_ERROR("VIDIOC_DQBUF failed: %s", strerror(errno));
        return nullptr;
    }

    uint32_t idx = buf.index;

    // 计算NV12总大小: Y plane + UV plane
    size_t y_size  = strides_[0] * height_;
    size_t uv_size = (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                     ? ((strides_[1] > 0) ? strides_[1] * height_ / 2 : y_size / 2)
                     : y_size / 2;
    size_t total   = y_size + uv_size;

    auto frame = std::make_shared<FrameBuffer>();
    frame->data       = (uint8_t*)malloc(total);
    frame->data_size  = total;
    frame->sequence   = sequence_++;
    frame->width      = width_;
    frame->height     = height_;
    frame->capture_ts = now();

    if (!frame->data) {
        LOG_ERROR("FrameBuffer malloc(%zu) failed", total);
        xioctl(VIDIOC_QBUF, &buf);
        return nullptr;
    }

    // 从mmap DMA buffer拷贝到FrameBuffer自有存储（约1ms @ 1080P DDR）
    if (buffer_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        memcpy(frame->data,
               static_cast<uint8_t*>(buffers_[idx]->start),
               y_size);
        memcpy(frame->data + y_size,
               static_cast<uint8_t*>(buffers_[idx]->start) + y_size,
               uv_size);
    } else {
        memcpy(frame->data,
               static_cast<uint8_t*>(buffers_[idx]->start), total);
    }

    // 拷贝完成后立即重新入队（驱动可继续使用该DMA buffer）
    xioctl(VIDIOC_QBUF, &buf);

    return frame;
}

} // namespace rk3568_vision
