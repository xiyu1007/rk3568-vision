#pragma once

#include "types.hpp"

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace rk3568_vision {

// ============================================================================
// RTMPPusher — RTMP 推流器 (基于 FFmpeg libavformat)
//
// 使用 FFmpeg libavformat 的 RTMP 协议实现。
// 支持自动重连: 断流后等待重连延迟，然后重新连接。
// ============================================================================
class RTMPPusher {
public:
    RTMPPusher();
    ~RTMPPusher();

    RTMPPusher(const RTMPPusher&) = delete;
    RTMPPusher& operator=(const RTMPPusher&) = delete;

    bool init(const std::string& url, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate);
    bool push_video_packet(const uint8_t* data, size_t size,
                           int64_t pts, bool keyframe);
    bool push_audio_packet(const uint8_t* data, size_t size, int64_t pts);

    bool reconnect();
    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

private:
    bool do_connect();
    void do_disconnect();

    std::string url_;
    uint32_t width_  = 1920;
    uint32_t height_ = 1080;
    uint32_t fps_    = 30;
    uint32_t bitrate_ = 4000000;

    std::atomic<bool> connected_{false};
    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream*        video_stream_ = nullptr;
};

} // namespace rk3568_vision
