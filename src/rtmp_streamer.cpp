// ============================================================================
// rtmp_streamer.cpp — RTMP 推流器实现
// ============================================================================

#include "vision/rtmp_streamer.hpp"

#include <chrono>
#include <thread>

#include "vision/logger.hpp"

namespace vision {

RtmpStreamer::RtmpStreamer(const StreamConfig& config) : config_(config) {}

RtmpStreamer::~RtmpStreamer() {
    Close();
}

// ---------------------------------------------------------------------------
// SetVideoParameters：设置视频参数 + SPS/PPS
// ---------------------------------------------------------------------------
void RtmpStreamer::SetVideoParameters(uint32_t width, uint32_t height, uint32_t fps,
                                      uint32_t bitrate, const uint8_t* extradata,
                                      int extradata_size) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;
    extradata_.assign(extradata, extradata + extradata_size);
}

// ---------------------------------------------------------------------------
// Open：打开 Muxer（首次或重连）
// ---------------------------------------------------------------------------
bool RtmpStreamer::Open() {
    return muxer_.Open("flv", config_.url, width_, height_, fps_, bitrate_,
                       extradata_.empty() ? nullptr : extradata_.data(),
                       static_cast<int>(extradata_.size()),
                       /*add_silent_audio=*/true);
}

// ---------------------------------------------------------------------------
// Push：推一个 H264 包（内部处理断线重连）
// ---------------------------------------------------------------------------
bool RtmpStreamer::Push(const PacketPtr& packet) {
    // 未打开时：按配置尝试重连。
    if (!muxer_.IsOpen()) {
        if (!config_.reconnect ||
            (config_.max_reconnect >= 0 && reconnect_count_ >= config_.max_reconnect)) {
            return false;
        }
        // 仅断线重连时等待；首次连接立即打开，否则会白白睡 2s，期间编码好的包
        // 堆积/丢旧，导致流起始时间戳跳变、拉流端缓冲。
        if (reconnect_count_ > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.reconnect_delay_ms));
        }
        if (!Open()) {
            ++reconnect_count_;
            return false;
        }
    }

    if (muxer_.Push(packet)) {
        reconnect_count_ = 0;
        return true;
    }
    // 写失败：关闭，下次 Push 时重连。
    muxer_.Close();
    return false;
}

// ---------------------------------------------------------------------------
// Close：关闭推流
// ---------------------------------------------------------------------------
void RtmpStreamer::Close() {
    muxer_.Close();
}

bool RtmpStreamer::IsOpen() const {
    return muxer_.IsOpen();
}

} // namespace vision
