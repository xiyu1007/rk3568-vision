// ============================================================================
// h264_encoder.hpp — H.264 编码器
// ============================================================================
//
// 把 NV12 帧编码为 H.264 码流（硬编 h264_rkmpp / 软编 libx264）。
// 编码产物经 on_packet 回调交出（一帧可能产 0~N 个包）。
//
// 设计要点：
//   - 软编时 NV12 → YUV420P 用 sws_scale 格式转换（libx264 需要 YUV420P）。
//   - 硬编时 NV12 零拷贝直通（h264_rkmpp 直接吃 NV12）。
// ============================================================================

#pragma once

#include <cstdint>
#include <functional>

#include "vision/config.hpp"
#include "vision/types.hpp"

struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct SwsContext;

namespace vision {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // 打开编码器。
    //   config：编码配置
    //   width/height：帧宽高
    //   fps：帧率（决定 time_base）
    bool Open(const EncodeConfig& config, uint32_t width, uint32_t height, uint32_t fps);

    // 编码一帧，产物经 on_packet 回调（一帧可能产 0~N 个包）。
    bool Encode(const FramePtr& frame,
                const std::function<void(const PacketPtr&)>& on_packet);

    // 冲刷编码器残留帧。
    void Flush(const std::function<void(const PacketPtr&)>& on_packet);

    // 关闭编码器并释放资源。
    void Close();

    bool IsHardware() const { return hardware_; }

    // 获取 SPS/PPS（extradata），供封装器使用。
    const uint8_t* GetExtradata() const;
    int GetExtradataSize() const;

private:
    AVCodecContext* codec_context_ = nullptr;
    const AVCodec* codec_ = nullptr;
    AVFrame* dst_frame_ = nullptr;       // 目标格式帧
    SwsContext* sws_context_ = nullptr;  // NV12→YUV420P（仅软编）
    bool hardware_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int64_t pts_ = 0;
};

} // namespace vision
