// ============================================================================
// mp4_recorder.cpp — 本地 MP4 录制器实现
// ============================================================================

#include "vision/mp4_recorder.hpp"

namespace vision {

Mp4Recorder::Mp4Recorder(const RecordConfig& config) : config_(config) {}

Mp4Recorder::~Mp4Recorder() {
    Close();
}

// ---------------------------------------------------------------------------
// Open：打开录制器（写 MP4 头）
// ---------------------------------------------------------------------------
bool Mp4Recorder::Open(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                       const uint8_t* extradata, int extradata_size) {
    return muxer_.Open("mp4", config_.path, width, height, fps, bitrate,
                       extradata, extradata_size,
                       /*add_silent_audio=*/false);
}

// ---------------------------------------------------------------------------
// Push：写入一个 H264 包
// ---------------------------------------------------------------------------
bool Mp4Recorder::Push(const PacketPtr& packet) {
    return muxer_.Push(packet);
}

// ---------------------------------------------------------------------------
// Close：关闭录制器（写 trailer）
// ---------------------------------------------------------------------------
void Mp4Recorder::Close() {
    muxer_.Close();
}

bool Mp4Recorder::IsOpen() const {
    return muxer_.IsOpen();
}

} // namespace vision
