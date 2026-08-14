// ============================================================================
// v4l2_capture.hpp — V4L2 视频采集（RAII 封装）
// ============================================================================
//
// 职责：打开 IMX415 摄像头，通过 V4L2 mmap 零拷贝方式采集 NV12 视频帧。
//
// 关键设计：
//   1. 【mmap 零拷贝】内核 DMA buffer 通过 mmap 映射到用户空间，采集不产生
//      内核→用户态的拷贝。
//   2. 【必要拷贝】mmap 的 DMA buffer 是循环复用的（归还后驱动立刻覆盖），
//      因此 read() 会把 NV12 数据拷到 Frame 自有的内存里，供下游异步处理。
//      这是全链路唯一一次不可避免的拷贝。
//   3. 【RAII】析构自动 STREAMOFF + munmap + close，异常/退出路径不泄漏。
//   4. 【poll 超时】read 内部用 poll 限时等待，使采集线程能周期性检查
//      running 标志，实现优雅退出。
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include "config.hpp"
#include "types.hpp"

namespace vision {

class V4l2Capture {
public:
    V4l2Capture() = default;
    ~V4l2Capture();

    V4l2Capture(const V4l2Capture&) = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    // 完整初始化：open → 查询能力 → 设置格式 → 设置帧率 → 申请并 mmap 缓冲区 → 入队。
    // 返回 false 表示初始化失败（设备不存在/格式不支持等）。
    bool open(const CaptureConfig& cfg);

    // 启动视频流（STREAMON）。
    bool start();

    // 阻塞读取一帧（内部 poll 限时 500ms）。
    // 成功返回 true 并填充 out；超时/错误返回 false。
    bool read(FramePtr& out);

    // 停止并释放所有资源（STREAMOFF + munmap + close）。
    void stop();

    // 访问器（流水线用它配置下游编码/推理）。
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t fps()    const { return fps_; }

private:
    // 单个 mmap 缓冲区（一个 NV12 帧 = Y 平面 + UV 平面）。
    struct MappedBuffer {
        std::vector<void*>  planes;   // 各平面 mmap 地址
        std::vector<size_t> lengths;  // 各平面 mmap 长度
    };

    int  xioctl(unsigned long request, void* arg);   // 带 EINTR 重试的 ioctl
    bool requestBuffers();                           // REQBUFS + QUERYBUF + mmap
    bool queueAll();                                 // 所有缓冲区入队

    int      fd_ = -1;                 // 设备文件描述符
    uint32_t width_ = 0, height_ = 0;  // 采集分辨率
    uint32_t fps_ = 0;                 // 采集帧率
    uint32_t stride_ = 0;              // Y 平面行步长（V4L2 bytesperline）
    uint32_t pixfmt_ = 0;              // 像素格式（NV12）
    uint32_t buf_type_ = 0;            // 缓冲区类型（MPLANE）
    uint32_t n_planes_ = 1;            // 平面数量（NV12 = 2）
    uint32_t n_buffers_ = 0;           // DMA buffer 数量
    std::vector<MappedBuffer> buffers_; // mmap 缓冲区数组
    bool started_ = false;             // 是否已 STREAMON
};

} // namespace vision
