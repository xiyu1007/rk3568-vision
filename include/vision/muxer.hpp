// ============================================================================
// muxer.hpp — 视频封装输出（FLV/RTMP、MP4）
// ============================================================================
//
// 把 H.264 包封装为 FLV（RTMP 推流）或 MP4（本地录制）。
//
// 设计要点：
//   - FLV 推流自动补一路静音 AAC 音轨（mediamtx v1.9.3 不转发纯视频流），
//     MP4 录制不需要。
//   - 静音 AAC 帧为硬编码字节（板端原生 AAC 编码器版本错配会段错误），
//     时间戳按视频时间戳对齐（写足帧），否则 mediamtx 只发头不发帧。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

#include "vision/types.hpp"

struct AVFormatContext;
struct AVStream;

namespace vision {

class Muxer {
public:
    Muxer() = default;
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // 打开封装器。
    //   format：封装格式（"flv" 推流 / "mp4" 录制）
    //   url：输出地址（rtmp:// 或文件路径）
    //   width/height/fps/bitrate：视频参数（写进流头）
    //   extradata/extradata_size：H264 SPS/PPS（AVCC 格式）
    //   add_silent_audio：是否补静音 AAC 音轨（FLV=true，MP4=false）
    bool Open(const std::string& format, const std::string& url,
              uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
              const uint8_t* extradata, int extradata_size, bool add_silent_audio);

    // 写入一个 H264 包（内部按时间戳交织封装）。
    bool Push(const PacketPtr& packet);

    // 关闭封装器并写 trailer。
    void Close();

    bool IsOpen() const { return context_ != nullptr; }

private:
    // 添加一路静音 AAC 音轨（仅 FLV）。
    void InitializeSilentAudio();

    // 写一路静音音频包（时间戳跟随视频对齐）。
    void WriteSilentAudio(int64_t video_pts);

    AVFormatContext* context_ = nullptr;
    AVStream* video_stream_ = nullptr;   // 视频流
    AVStream* audio_stream_ = nullptr;   // 静音 AAC 音频流
    uint32_t fps_ = 25;
    int64_t audio_pts_ = 0;              // 音频时间戳（样本数，每帧 +1024）
};

} // namespace vision
