// ============================================================================
// encoder.hpp — H.264 编码 + 封装输出（RTMP 推流 / MP4 录制）
// ============================================================================
//
// 两个类，同属“输出”环节：
//   H264Encoder —— 把 NV12 帧编码为 H.264 码流（硬编 h264_rkmpp / 软编 libx264）
//   Muxer       —— 把 H.264 包封装为 FLV(RTMP) 或 MP4(本地文件)
// ============================================================================

#pragma once

#include <functional>
#include <string>

#include "common.hpp"

struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct SwsContext;
struct AVFormatContext;
struct AVStream;

namespace vision {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool open(const EncodeConfig& cfg, uint32_t width, uint32_t height, uint32_t fps);
    // 编码一帧，产物经 on_packet 回调（一帧可能产 0~N 个包）。
    bool encode(const FramePtr& frame,
                const std::function<void(const PacketPtr&)>& on_packet);
    void flush(const std::function<void(const PacketPtr&)>& on_packet);
    void close();

    bool hardware() const { return hardware_; }
    const uint8_t* extradata() const;   // SPS/PPS，供 Muxer 使用
    int extradata_size() const;

private:
    AVCodecContext* ctx_       = nullptr;
    const AVCodec*  codec_     = nullptr;
    AVFrame*        dst_frame_ = nullptr;   // 目标格式帧
    SwsContext*     sws_       = nullptr;   // NV12→YUV420P（仅软编）
    bool     hardware_ = false;
    uint32_t width_ = 0, height_ = 0;
    int64_t  pts_ = 0;
};

class Muxer {
public:
    Muxer() = default;
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // format="flv" 推流 / "mp4" 录制；url=rtmp:// 或文件路径。
    bool open(const std::string& format, const std::string& url,
              uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
              const uint8_t* extradata, int extradata_size);
    bool push(const PacketPtr& pkt);
    void close();
    bool isOpen() const { return ctx_ != nullptr; }

private:
    // 为 FLV/RTMP 推流补一路静音 AAC（原因见 encoder.cpp 的 initSilentAudio）。
    void initSilentAudio();
    void writeSilentAudio(int64_t pts);

    AVFormatContext* ctx_ = nullptr;
    AVStream*        vs_  = nullptr;    // 视频流
    AVStream*        as_  = nullptr;    // 静音 AAC 音频流
    uint32_t         fps_ = 25;
    int64_t          audio_pts_ = 0;    // 音频时间戳（样本数，每帧 +1024）
};

} // namespace vision
