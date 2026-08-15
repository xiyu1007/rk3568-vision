// ============================================================================
// camera_source.hpp — 视频采集（V4L2 摄像头 / mp4 文件）
// ============================================================================
//
// 一个 CameraSource 类统一两种输入，由配置 source 决定：
//   source=v4l2 → 打开 /dev/videoX，mmap + EXPBUF 导出 dmabuf，零拷贝采集 NV12
//   source=mp4  → 用 FFmpeg 解码 data/*.mp4 转 NV12（无摄像头时联调用）
//
// 设计要点（借鉴参考项目 around_view_app 的 CameraManager）：
//   1. 【回调解耦】采集线程通过 RegisterFrameCallback 注册的回调把帧交给上层，
//      回调里只做入队，不做重活（推理/编码都在上层协调器里）。
//   2. 【dmabuf 零拷贝】V4L2 采集不 memcpy 到 CPU 内存，而是 EXPBUF 导出 dmabuf fd
//      直接放进 Frame；下游（RGA）用 importbuffer_fd 消费，全程零拷贝。
//   3. 【生命周期】dmabuf 帧的 shared_ptr 用自定义 deleter，引用计数归零时
//      QBUF 归还 buffer，避免"buffer 未归还导致采集饿死"或"重复归还崩溃"。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "vision/config.hpp"
#include "vision/types.hpp"

// FFmpeg 前向声明（仅 mp4 输入用到，避免在头文件暴露 FFmpeg 细节）。
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct SwsContext;
struct AVPacket;

namespace vision {

class CameraSource {
public:
    CameraSource() = default;
    ~CameraSource();

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    // 按配置初始化采集（打开 V4L2 设备或 mp4 文件）。
    bool Initialize(const CaptureConfig& config);

    // 启动采集线程。
    bool Start();

    // 停止采集线程并释放资源（幂等）。
    void Stop();

    // 注册帧回调（上层协调器调用）。采集线程回调里只入队，不做重活。
    void RegisterFrameCallback(std::function<void(FramePtr)> callback);

    // 查询采集参数（在 Initialize 后有效）。
    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetFrameRate() const { return frame_rate_; }
    bool IsMp4Source() const { return is_mp4_source_; }

private:
    // ---- V4L2 采集 ----
    bool InitializeV4l2();
    bool RequestV4l2Buffers();                  // REQBUFS + mmap + EXPBUF
    void CaptureLoopV4l2();
    void ReturnBuffer(uint32_t buffer_index);   // QBUF 归还 DMA buffer

    // ---- mp4 解码 ----
    bool InitializeMp4();
    void CaptureLoopMp4();

    // 回调分发（采集线程内调用）。
    void InvokeCallback(FramePtr frame);

    CaptureConfig config_;
    bool is_mp4_source_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frame_rate_ = 0;

    std::atomic<bool> running_{false};
    std::thread capture_thread_;

    // 帧回调（单回调场景，用 mutex 保护注册与分发）。
    std::function<void(FramePtr)> frame_callback_;
    std::mutex callback_mutex_;

    // ---- V4L2 状态 ----
    int video_fd_ = -1;                       // V4L2 设备文件描述符
    uint32_t buffer_type_ = 0;                // V4L2 buffer 类型（单平面/多平面）
    uint32_t pixel_format_ = 0;               // 像素格式（NV12）
    uint32_t plane_count_ = 1;                // 平面数量
    uint32_t stride_ = 0;                     // 行字节跨度（bytesperline）
    uint32_t buffer_count_ = 0;               // DMA buffer 数量
    bool stream_started_ = false;             // STREAMON 是否已调用

    struct MappedBuffer {
        void* address = nullptr;              // mmap 映射地址
        size_t length = 0;                    // 映射长度
    };
    std::vector<MappedBuffer> mapped_buffers_; // mmap 映射（EXPBUF 需要）
    std::vector<int> dma_fds_;                // 每个 buffer 的 dmabuf fd（EXPBUF 导出）
    std::mutex buffer_mutex_;                 // 保护 DQBUF/QBUF ioctl 序列

    // ---- mp4 状态（FFmpeg）----
    AVFormatContext* format_context_ = nullptr;
    AVCodecContext* codec_context_ = nullptr;
    const AVCodec* codec_ = nullptr;
    int video_stream_index_ = -1;
    AVFrame* decoded_frame_ = nullptr;
    AVFrame* nv12_frame_ = nullptr;
    SwsContext* sws_context_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint64_t frame_interval_us_ = 40000;      // 帧间隔（微秒）
    uint64_t frame_index_ = 0;                // 已输出帧序号
    uint64_t start_time_us_ = 0;              // 采集起始时间戳
};

} // namespace vision
