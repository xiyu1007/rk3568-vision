#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <chrono>
#include <string>
#include <memory>
#include <atomic>
#include <opencv2/core.hpp>

namespace rk3568_vision {

using Timestamp = std::chrono::steady_clock::time_point;

inline int64_t timestamp_to_ms(Timestamp ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        ts.time_since_epoch()).count();
}

inline Timestamp now() { return std::chrono::steady_clock::now(); }

struct FrameBuffer {
    uint8_t*  data      = nullptr;
    size_t    data_size = 0;

    // BGR缓存 — 由inference_loop填充，display_loop复用
    cv::Mat   bgr;

    uint32_t   width      = 0;
    uint32_t   height     = 0;
    uint64_t   sequence   = 0;
    Timestamp  capture_ts;
    Timestamp  infer_ts;

    ~FrameBuffer() { free(data); }
};

using FramePtr = std::shared_ptr<FrameBuffer>;

struct DetectBox {
    int32_t  x          = 0;
    int32_t  y          = 0;
    int32_t  width      = 0;
    int32_t  height     = 0;
    int32_t  class_id   = -1;
    float    confidence = 0.0f;
    char     label[64]  = {0};
};

struct DetectResult {
    uint32_t   count = 0;
    DetectBox  boxes[64];
};

enum class ErrorCode : int32_t {
    SUCCESS            = 0,
    ERR_V4L2_OPEN      = -1001,
    ERR_V4L2_FORMAT    = -1002,
    ERR_V4L2_MMAP      = -1003,
    ERR_V4L2_STREAM    = -1004,
    ERR_V4L2_DQBUF     = -1005,
    ERR_RKNN_INIT      = -2001,
    ERR_RKNN_INFER     = -2002,
    ERR_ENCODE_INIT    = -3001,
    ERR_ENCODE_FRAME   = -3002,
    ERR_STREAM_CONNECT = -4001,
    ERR_STREAM_SEND    = -4002,
    ERR_INVALID_PARAM  = -5001,
};

} // namespace rk3568_vision
