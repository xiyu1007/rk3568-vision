// ============================================================================
// mp4_recorder.hpp — 本地 MP4 录制器
// ============================================================================
//
// 把 H.264 包封装为 MP4 写到本地文件。与 RTMP 推流的区别：
//   - 封装格式为 mp4（无静音 AAC 音轨）。
//   - 无断线重连（本地文件写失败即停止）。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

#include "vision/config.hpp"
#include "vision/muxer.hpp"
#include "vision/types.hpp"

namespace vision {

class Mp4Recorder {
public:
    explicit Mp4Recorder(const RecordConfig& config);
    ~Mp4Recorder();

    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;

    // 打开录制器（写 MP4 头）。
    bool Open(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
              const uint8_t* extradata, int extradata_size);

    // 写入一个 H264 包。
    bool Push(const PacketPtr& packet);

    // 关闭录制器（写 trailer）。
    void Close();

    bool IsOpen() const;

private:
    RecordConfig config_;
    Muxer muxer_;
};

} // namespace vision
