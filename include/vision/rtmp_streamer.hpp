// ============================================================================
// rtmp_streamer.hpp — RTMP 推流器（FLV 封装 + 断线重连）
// ============================================================================
//
// 把 H.264 包封装为 FLV 并通过 RTMP 推流到 mediamtx/nginx-rtmp。
// 断线后按配置自动重连（有界重连次数）。
//
// 设计要点：
//   - 内部持有一个 Muxer（FLV + 静音 AAC），视频参数在首帧编码后设置。
//   - Push 失败时关闭 Muxer，下次 Push 时自动重连。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vision/config.hpp"
#include "vision/muxer.hpp"
#include "vision/types.hpp"

namespace vision {

class RtmpStreamer {
public:
    explicit RtmpStreamer(const StreamConfig& config);
    ~RtmpStreamer();

    RtmpStreamer(const RtmpStreamer&) = delete;
    RtmpStreamer& operator=(const RtmpStreamer&) = delete;

    // 设置视频参数 + SPS/PPS（首帧编码后调用一次）。
    void SetVideoParameters(uint32_t width, uint32_t height, uint32_t fps,
                            uint32_t bitrate, const uint8_t* extradata,
                            int extradata_size);

    // 推一个 H264 包（内部处理断线重连）。
    bool Push(const PacketPtr& packet);

    // 关闭推流（写 trailer + 释放）。
    void Close();

    bool IsOpen() const;

private:
    // 打开 Muxer（首次或重连时）。
    bool Open();

    StreamConfig config_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 0;
    uint32_t bitrate_ = 0;
    std::vector<uint8_t> extradata_;   // SPS/PPS 副本（重连用）
    Muxer muxer_;
    int reconnect_count_ = 0;          // 已重连次数
};

} // namespace vision
