// ============================================================================
// config.hpp — 配置结构体与加载接口
// ============================================================================
//
// 与 conf/default.json 中的配置项一一对应。
// 加载优先级：内置默认值 < JSON 文件 < 命令行参数（在 main.cpp 中覆盖）。
//
// JSON 解析使用 nlohmann/json（third_lib/json/json.hpp），
// 文件不存在时降级使用默认值并打印告警，方便无配置文件时直接调试。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace vision {

// ---------------------------------------------------------------------------
// 视频采集（V4L2）配置
// ---------------------------------------------------------------------------
struct CaptureConfig {
    std::string device        = "/dev/video0";  // V4L2 设备节点（IMX415 MIPI 摄像头）
    uint32_t    width         = 1280;           // 采集宽度（像素）
    uint32_t    height        = 720;            // 采集高度（像素）
    uint32_t    fps           = 25;             // 目标帧率（720P@25fps）
    std::string pixel_format  = "NV12";         // 像素格式：NV12（ISP 直出，推荐）
    uint32_t    buffer_count  = 6;              // V4L2 DMA buffer 数量（4~8，建议 6）
    bool        use_mplane    = true;           // 是否使用多平面（MPLANE）API
};

// ---------------------------------------------------------------------------
// RKNN 推理配置
// ---------------------------------------------------------------------------
struct InferenceConfig {
    bool        enabled        = true;                 // 是否启用推理
    std::string backend        = "auto";               // auto / rknn / mock
    std::string model_path     = "model/yolov5s.rknn"; // RKNN 模型路径
    std::string labels_path    = "model/coco_80_labels_list.txt"; // 类别标签
    float       conf_threshold = 0.25f;                // 置信度阈值
    float       nms_threshold  = 0.45f;                // NMS IoU 阈值
    uint32_t    model_width    = 640;                  // 模型输入宽（YOLOv5 固定 640）
    uint32_t    model_height   = 640;                  // 模型输入高
    uint32_t    npu_core       = 0;                    // NPU 核心号（RK3568 为 0）
};

// ---------------------------------------------------------------------------
// 稳帧器（FramePacer）配置
// ---------------------------------------------------------------------------
struct PacerConfig {
    bool     enabled        = true;    // 是否启用稳帧（帧率稳定节拍）
    uint32_t target_fps     = 25;      // 稳定输出的目标帧率
    bool     allow_duplicate = true;   // 源帧缺帧时是否补帧（复制上一帧）
    uint32_t queue_depth    = 8;       // 稳帧输出队列深度（inf_q）
};

// ---------------------------------------------------------------------------
// H.264 编码配置
// ---------------------------------------------------------------------------
struct EncodeConfig {
    bool        enabled  = true;       // 是否启用编码
    bool        hardware = true;       // true=硬件编码(h264_rkmpp，仅 aarch64)
                                       // false=软件编码(libx264)
    std::string codec    = "h264";     // 编码格式
    uint32_t    bitrate  = 4000000;    // 码率（bps），720P@25fps 建议 2~4M
    uint32_t    gop_size = 50;         // GOP 关键帧间隔（帧数）
    std::string preset   = "fast";     // libx264 preset（软编时生效）
    std::string profile  = "high";     // libx264 profile（软编时生效）
};

// ---------------------------------------------------------------------------
// RTMP 推流配置
// ---------------------------------------------------------------------------
struct StreamConfig {
    bool        enabled           = true;      // 是否启用推流
    std::string url               = "rtmp://127.0.0.1/live/stream"; // 服务器地址
    bool        reconnect         = true;      // 断流自动重连
    uint32_t    reconnect_delay_ms = 2000;     // 重连间隔（毫秒）
    int32_t     max_reconnect     = -1;        // 最大重连次数（-1 无限）
};

// ---------------------------------------------------------------------------
// 本地 MP4 录制配置
// ---------------------------------------------------------------------------
struct RecordConfig {
    bool        enabled        = false;              // 是否启用录制（一路保存 MP4）
    std::string path           = "output/record.mp4"; // 输出文件路径
    uint32_t    segment_seconds = 0;                 // 分段时长（秒），0=单文件
};

// ---------------------------------------------------------------------------
// 系统监控配置
// ---------------------------------------------------------------------------
struct MonitorConfig {
    bool     enabled         = true;    // 是否启用监控（CPU/内存/温度/健康）
    uint32_t log_interval_ms = 5000;    // 状态日志输出间隔（毫秒）
};

// ---------------------------------------------------------------------------
// 日志配置
// ---------------------------------------------------------------------------
struct LogConfig {
    std::string level        = "info";              // debug / info / warn / error
    std::string file         = "log/rk3568_vision.log"; // 日志文件路径
    bool        console      = true;                // 是否同时输出到控制台
    bool        async        = true;                // 是否异步写（推荐，不阻塞业务线程）
    uint64_t    max_size     = 10 * 1024 * 1024;    // 单文件最大字节（触发轮转）
    uint32_t    backup_count = 3;                   // 轮转保留份数
};

// ---------------------------------------------------------------------------
// 总配置
// ---------------------------------------------------------------------------
struct Config {
    CaptureConfig   capture;
    InferenceConfig inference;
    PacerConfig     pacer;
    EncodeConfig    encode;
    StreamConfig    stream;
    RecordConfig    record;
    MonitorConfig   monitor;
    LogConfig       log;
};

// ---------------------------------------------------------------------------
// 接口
// ---------------------------------------------------------------------------

// 从 JSON 文件加载配置。文件不存在或解析失败时返回 false（但仍返回默认值）。
// cfg 在调用前应已用默认值初始化。
bool loadConfig(const std::string& path, Config& cfg);

} // namespace vision
