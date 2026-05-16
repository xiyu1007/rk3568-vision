#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

#include "types.hpp"
#include "logger.hpp"

namespace rk3568_vision {

// ============================================================================
// V4L2Buffer — 封装单个 mmap buffer
// 使用 RAII: 构造时 mmap，析构时 munmap
// ============================================================================
struct V4L2Buffer {
    void*   start = nullptr;
    size_t  length = 0;
    int     index = -1;
    int     fd = -1;
    off_t   offset = 0;

    V4L2Buffer() = default;
    V4L2Buffer(int fd, size_t len, off_t off, int idx)
        : length(len), index(idx), fd(fd), offset(off) {
        start = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    }

    ~V4L2Buffer() {
        if (start && start != MAP_FAILED) {
            munmap(start, length);
        }
    }

    V4L2Buffer(const V4L2Buffer&) = delete;
    V4L2Buffer& operator=(const V4L2Buffer&) = delete;

    V4L2Buffer(V4L2Buffer&& other) noexcept
        : start(other.start), length(other.length)
        , index(other.index), fd(other.fd), offset(other.offset) {
        other.start = nullptr;
    }

    V4L2Buffer& operator=(V4L2Buffer&& other) noexcept {
        if (this != &other) {
            if (start && start != MAP_FAILED) munmap(start, length);
            start = other.start; length = other.length;
            index = other.index; fd = other.fd; offset = other.offset;
            other.start = nullptr;
        }
        return *this;
    }

    bool valid() const { return start && start != MAP_FAILED; }
};

// ============================================================================
// V4L2Capture — V4L2 视频采集类
//
// 设计要点:
// - 使用 O_NONBLOCK + epoll: 避免 DQBUF 阻塞，精确控制帧率
// - Multi-planar 支持: NV12 需要 2 个 plane (Y + UV)
// - 零拷贝: mmap 映射内核 DMA buffer，直接传递给下游
// - FrameBuffer 池: 预分配 FrameBuffer，复用避免频繁 new/delete
// - 自动重连: 设备断开时尝试重新打开
// ============================================================================
class V4L2Capture {
public:
    using FrameCallback = std::function<void(std::shared_ptr<FrameBuffer>)>;

    V4L2Capture();
    ~V4L2Capture();

    // 禁止拷贝
    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    bool open(const std::string& device, uint32_t width, uint32_t height,
              uint32_t fps, const std::string& pixfmt, uint32_t buf_count);
    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // 捕获一帧（非阻塞，失败返回 nullptr）
    std::shared_ptr<FrameBuffer> capture();

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t fps()    const { return fps_; }

private:
    bool init_device();
    bool init_format();
    bool init_fps();
    bool init_mmap(uint32_t buf_count);
    bool queue_all_buffers();
    int  xioctl(int request, void* arg);

    int              fd_ = -1;
    int              epoll_fd_ = -1;
    std::string      device_;
    uint32_t         width_ = 1920, height_ = 1080, fps_ = 30;
    uint32_t         pixel_format_ = 0;
    uint32_t         buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    uint32_t         num_planes_ = 2;
    uint32_t         strides_[4] = {0};
    uint32_t         sizes_[4] = {0};
    std::atomic<bool> running_{false};
    std::vector<std::unique_ptr<V4L2Buffer>> buffers_;
    uint64_t         sequence_ = 0;
};

} // namespace rk3568_vision
