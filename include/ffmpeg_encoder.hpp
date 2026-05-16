#pragma once

#include "types.hpp"

#include <string>
#include <memory>
#include <atomic>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace rk3568_vision {

// ============================================================================
// FFmpegEncoder — H.264/H.265 硬件编码器
//
// 使用 FFmpeg libavcodec API 进行视频编码。
// RK3568 可能支持 MPP 硬件编码（通过 h264_rkmpp / h265_rkmpp），
// 如果未安装 MPP FFmpeg 插件，回退到 libx264 软件编码。
// ============================================================================
class FFmpegEncoder {
public:
    using PacketCallback = std::function<void(const uint8_t* data, size_t size,
                                              int64_t pts, bool keyframe)>;

    FFmpegEncoder();
    ~FFmpegEncoder();

    FFmpegEncoder(const FFmpegEncoder&) = delete;
    FFmpegEncoder& operator=(const FFmpegEncoder&) = delete;

    bool init(uint32_t width, uint32_t height, uint32_t fps,
              uint32_t bitrate, uint32_t gop_size,
              const std::string& codec = "h264",
              const std::string& preset = "fast");
    bool encode(const uint8_t* nv12_data, int64_t pts = -1);
    bool flush();

    void set_packet_callback(PacketCallback cb) { packet_cb_ = std::move(cb); }
    bool is_initialized() const { return initialized_; }

private:
    bool initialized_ = false;

    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame*        frame_     = nullptr;
    AVPacket*       packet_    = nullptr;
    SwsContext*     sws_       = nullptr;

    uint32_t width_  = 1920;
    uint32_t height_ = 1080;
    int64_t  frame_counter_ = 0;

    PacketCallback packet_cb_;
};

} // namespace rk3568_vision
