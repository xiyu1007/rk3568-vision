// ============================================================================
// capture.hpp — 视频采集（V4L2 摄像头 / mp4 文件）
// ============================================================================
//
// 一个 Capture 类统一两种输入，由配置 source 决定：
//   source=v4l2 → 打开 /dev/videoX（IMX415，mmap 零拷贝采集 NV12）
//   source=mp4  → 用 FFmpeg 解码 data/*.mp4 转 NV12（无摄像头时联调用）
//
// 二者都输出紧密排列的 NV12 帧（Frame），对下游完全透明。
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include "common.hpp"

// FFmpeg 前向声明（仅 mp4 输入用到，避免在头文件暴露 FFmpeg 细节）。
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct SwsContext;
struct AVPacket;

namespace vision {

class Capture {
public:
    Capture() = default;
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool     open(const CaptureConfig& cfg);   // 按 source 选择 v4l2/mp4 并初始化
    bool     start();                           // 开始采集
    bool     read(FramePtr& out);               // 读取一帧 NV12
    void     stop();                            // 停止并释放资源

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t fps()    const { return fps_; }

private:
    // ---- V4L2 ----
    bool openV4l2();
    bool readV4l2(FramePtr& out);
    void stopV4l2();
    int  xioctl(unsigned long req, void* arg);
    bool requestBuffers();
    bool queueAll();

    // ---- mp4 ----
    bool openMp4();
    bool readMp4(FramePtr& out);
    void stopMp4();

    CaptureConfig cfg_;
    bool     is_mp4_ = false;
    uint32_t width_ = 0, height_ = 0, fps_ = 25;

    // V4L2 状态
    struct MappedBuffer {
        std::vector<void*>  planes;
        std::vector<size_t> lengths;
    };
    int      fd_ = -1;
    uint32_t stride_ = 0, pixfmt_ = 0, buf_type_ = 0;
    uint32_t n_planes_ = 1, n_buffers_ = 0;
    std::vector<MappedBuffer> buffers_;
    bool started_ = false;

    // mp4 状态
    AVFormatContext* fmt_ctx_  = nullptr;
    AVCodecContext*  codec_ctx_ = nullptr;
    const AVCodec*   codec_     = nullptr;
    int      video_idx_ = -1;
    AVFrame* dec_frame_  = nullptr;
    AVFrame* nv12_frame_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVPacket* pkt_  = nullptr;
    uint64_t frame_interval_us_ = 40000;
    uint64_t frame_idx_ = 0, start_us_ = 0;
};

} // namespace vision
