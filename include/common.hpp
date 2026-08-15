// ============================================================================
// common.hpp — 公共类型、常量、配置结构体
// ============================================================================
//
// 全项目的公共定义集中在此，避免类型散落多个头文件：
//   - 时间戳、帧、检测结果
//   - 编码包（AVPacket 共享指针）
//   - 全部配置结构体（与 conf/default.json 一一对应）
// ============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>   // AVPacket（编码包类型）
}

namespace vision {

// ---------------------------------------------------------------------------
// 时间戳
// ---------------------------------------------------------------------------
using TimestampUs = uint64_t;

// 当前单调时钟（微秒），用于测延迟。
inline TimestampUs nowUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

// ---------------------------------------------------------------------------
// 检测结果
// ---------------------------------------------------------------------------
constexpr int kMaxBoxes    = 64;    // 单帧最多检测框数
constexpr int kMaxLabelLen = 64;    // 类别标签最大长度

struct DetectBox {
    int   x = 0, y = 0, w = 0, h = 0;   // 框在原图像素坐标系
    int   class_id = -1;
    float conf = 0.0f;
    char  label[kMaxLabelLen] = {0};
};

struct DetectResult {
    uint32_t  count = 0;
    DetectBox boxes[kMaxBoxes];
};

// ---------------------------------------------------------------------------
// 帧（流水线核心数据单元）
// ---------------------------------------------------------------------------
struct Frame {
    uint64_t   seq = 0;                     // 帧序号
    uint32_t   width = 0, height = 0;       // 分辨率
    TimestampUs capture_ts = 0;             // 采集完成时间戳
    TimestampUs inference_ts = 0;           // 推理完成时间戳
    std::vector<uint8_t> nv12;              // NV12 数据（w*h*1.5）
    DetectResult detect;                    // 检测结果
};
using FramePtr = std::shared_ptr<Frame>;

inline FramePtr makeFrame(uint32_t w, uint32_t h) {
    auto f = std::make_shared<Frame>();
    f->width = w; f->height = h;
    f->nv12.resize(static_cast<size_t>(w) * h * 3 / 2);
    return f;
}

// 深拷贝一帧（稳帧器补帧用，避免与下游共享同一块 NV12 造成竞争）。
inline FramePtr cloneFrame(const FramePtr& src) {
    auto f = std::make_shared<Frame>();
    f->seq = src->seq; f->width = src->width; f->height = src->height;
    f->capture_ts = src->capture_ts; f->inference_ts = src->inference_ts;
    f->nv12 = src->nv12;
    return f;
}

// ---------------------------------------------------------------------------
// 编码包（AVPacket 共享指针，自动引用计数）
// ---------------------------------------------------------------------------
struct PacketDeleter {
    void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};
using PacketPtr = std::shared_ptr<AVPacket>;

inline PacketPtr makePacket() {
    return PacketPtr(av_packet_alloc(), PacketDeleter{});
}

// ---------------------------------------------------------------------------
// 配置结构体（与 conf/default.json 对应）
// ---------------------------------------------------------------------------
struct CaptureConfig {
    std::string source = "v4l2";           // v4l2=摄像头 / mp4=视频文件
    std::string device = "/dev/video0";    // V4L2 设备节点
    std::string file   = "data/test.mp4";  // mp4 文件路径
    uint32_t    width  = 1280, height = 720, fps = 25;
    uint32_t    buffer_count = 6;          // V4L2 DMA buffer 数量
    bool        use_mplane   = true;       // 是否用多平面 API
};

struct InferenceConfig {
    bool        enabled        = true;
    std::string model_path     = "model/yolov5s.rknn";
    std::string labels_path    = "model/coco_80_labels_list.txt";
    float       conf_threshold = 0.25f;
    float       nms_threshold  = 0.45f;
    uint32_t    model_width    = 640, model_height = 640;
    uint32_t    npu_core       = 0;        // RK3568 固定 0
    bool        use_sigmoid    = true;     // 后处理是否 sigmoid：标准 yolov5s=true，relu 版=false
};

struct PacerConfig {
    uint32_t target_fps      = 25;
    bool     allow_duplicate = true;       // 缺帧时是否补帧
};

struct EncodeConfig {
    bool        hardware = true;           // true=h264_rkmpp 硬编 / false=libx264
    uint32_t    bitrate  = 4000000;
    uint32_t    gop_size = 50;
    std::string preset   = "fast";         // 软编 preset
    std::string profile  = "high";         // 软编 profile
};

struct StreamConfig {
    bool        enabled           = true;
    std::string url               = "rtmp://127.0.0.1/live/stream";
    bool        reconnect         = true;
    uint32_t    reconnect_delay_ms = 2000;
    int32_t     max_reconnect     = -1;    // -1 无限
};

struct RecordConfig {
    bool        enabled = false;
    std::string path    = "output/record.mp4";
};

struct LogConfig {
    std::string level        = "info";     // debug/info/warn/error
    std::string file         = "log/rk3568_vision.log";
    bool        console      = true;
    bool        async        = true;
    uint64_t    max_size     = 10 * 1024 * 1024;
    uint32_t    backup_count = 3;
};

struct Config {
    CaptureConfig   capture;
    InferenceConfig inference;
    PacerConfig     pacer;
    EncodeConfig    encode;
    StreamConfig    stream;
    RecordConfig    record;
    LogConfig       log;
    uint32_t        monitor_interval_ms = 5000;   // 监控日志间隔
};

// 从 JSON 文件加载配置（实现在 main.cpp）。
bool loadConfig(const std::string& path, Config& cfg);

} // namespace vision
