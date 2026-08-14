// ============================================================================
// encoder.hpp — H.264 编码器（FFmpeg，硬编/软编可选）
// ============================================================================
//
// 职责：把 NV12 视频帧编码为 H.264 码流，输出为 AVPacket 序列。
//
// 编码器选择（由配置 encode.hardware 决定）：
//   - hardware=true  → h264_rkmpp（RK3568 MPP 硬件编码器，仅 aarch64）
//   - hardware=false → libx264  （软件编码器，全平台可用，x86 默认）
//
// 关键设计：
//   - 【零 B 帧】设置 max_b_frames=0，消除编码端参考延迟，降低端到端延迟。
//   - 【NV12 直通】硬编路径直接把 NV12 数据交给编码器（零拷贝，无格式转换）；
//     软编路径用 swscale 把 NV12 转成 YUV420P（libx264 要求的平面格式）。
//   - 【一次多包】avcodec_send_frame 后可能一次吐出 0~N 个包，通过回调逐个
//     交给下游，由调用方决定推流/录制。
// ============================================================================

#pragma once

#include <functional>

#include "config.hpp"
#include "encoded_packet.hpp"
#include "types.hpp"

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace vision {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // 初始化编码器。width/height/fps 来自采集（编码分辨率跟随采集）。
    bool open(const EncodeConfig& cfg, uint32_t width, uint32_t height,
              uint32_t fps);

    // 编码一帧，产生的每个 H.264 包通过 on_packet 回调交给下游。
    bool encode(const FramePtr& frame,
                const std::function<void(const PacketPtr&)>& on_packet);

    // 冲刷编码器内部缓冲（关闭前调用，取出尚未输出的帧）。
    void flush(const std::function<void(const PacketPtr&)>& on_packet);

    // 释放编码器资源。
    void close();

    // 是否使用硬件编码器。
    bool hardware() const { return hardware_; }

    // 编码器 extradata（SPS/PPS），供推流/录制设置 codecpar。
    const uint8_t* extradata() const;
    int            extradata_size() const;

private:
    AVCodecContext* ctx_       = nullptr;   // 编码器上下文
    const AVCodec*  codec_     = nullptr;   // 编码器描述
    AVFrame*        dst_frame_ = nullptr;   // 目标格式帧（软编：YUV420P / 硬编：NV12）
    SwsContext*     sws_       = nullptr;   // NV12→YUV420P 转换（仅软编）

    bool     hardware_ = false;
    uint32_t width_    = 0;
    uint32_t height_   = 0;
    int64_t  pts_      = 0;    // 帧时间戳计数器（以 1/fps 为单位）
};

} // namespace vision
