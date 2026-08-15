// ============================================================================
// config.hpp — 配置结构体
// ============================================================================
//
// 全部配置结构体集中在此，与 conf/default.yaml 一一对应。
// 由 config_loader（YAML 解析器）从配置文件加载，命令行参数可覆盖。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace vision {

// ---------------------------------------------------------------------------
// 采集配置
// ---------------------------------------------------------------------------
struct CaptureConfig {
    std::string source = "v4l2";          // 采集源：v4l2（摄像头）/ mp4（视频文件）
    std::string device = "/dev/video0";   // V4L2 设备节点
    std::string file = "data/test.mp4";   // mp4 文件路径
    uint32_t width = 1280;                // 采集宽度
    uint32_t height = 720;                // 采集高度
    uint32_t fps = 25;                    // 采集帧率
    uint32_t buffer_count = 6;            // V4L2 DMA buffer 数量
    bool use_multi_planar = true;         // 是否使用多平面 V4L2 API
};

// ---------------------------------------------------------------------------
// 推理配置
// ---------------------------------------------------------------------------
struct InferenceConfig {
    bool enabled = true;                    // 是否启用推理
    std::string model_path = "model/yolov5s.rknn";  // RKNN 模型路径
    std::string labels_path = "model/coco_80_labels_list.txt";  // 类别标签文件
    float confidence_threshold = 0.25f;    // 置信度阈值
    float nms_threshold = 0.45f;           // NMS 交并比阈值
    uint32_t model_width = 640;            // 模型输入宽
    uint32_t model_height = 640;           // 模型输入高
    uint32_t npu_core = 0;                 // NPU 核心（RK3568 固定 0）
    bool use_sigmoid = true;               // 后处理是否 sigmoid：
                                           //   标准 yolov5s = true（输出是 logits）
                                           //   relu 版 = false（输出已是 sigmoid 后值）
};

// ---------------------------------------------------------------------------
// 稳帧器配置
// ---------------------------------------------------------------------------
struct PacerConfig {
    uint32_t target_fps = 25;              // 目标输出帧率
    bool allow_duplicate = true;           // 缺帧时是否复制上一帧补帧
};

// ---------------------------------------------------------------------------
// 编码配置
// ---------------------------------------------------------------------------
struct EncodeConfig {
    bool hardware = true;                  // true=h264_rkmpp 硬编 / false=libx264 软编
    uint32_t bitrate = 4000000;            // 码率（bps）
    uint32_t gop_size = 50;                // 关键帧间隔（帧数）
    std::string preset = "fast";           // 软编 preset（x264）
    std::string profile = "high";          // 软编 profile（x264）
};

// ---------------------------------------------------------------------------
// RTMP 推流配置
// ---------------------------------------------------------------------------
struct StreamConfig {
    bool enabled = true;                   // 是否启用推流
    std::string url = "rtmp://127.0.0.1/live/stream";  // RTMP 地址
    bool reconnect = true;                 // 断线是否重连
    uint32_t reconnect_delay_ms = 2000;    // 重连间隔
    int32_t max_reconnect = -1;            // 最大重连次数（-1 无限）
};

// ---------------------------------------------------------------------------
// 本地录制配置
// ---------------------------------------------------------------------------
struct RecordConfig {
    bool enabled = false;                  // 是否启用录制
    std::string path = "output/record.mp4";  // 录制文件路径
};

// ---------------------------------------------------------------------------
// 日志配置
// ---------------------------------------------------------------------------
struct LogConfig {
    std::string level = "info";            // 日志级别：debug/info/warn/error
    std::string file = "log/rk3568_vision.log";  // 日志文件路径
    bool console = true;                   // 是否输出到控制台
    bool async = true;                     // 是否异步写（后台线程落盘）
    uint64_t max_size = 10 * 1024 * 1024;  // 单文件最大字节数（超过轮转）
    uint32_t backup_count = 3;             // 轮转保留的备份文件数
};

// ---------------------------------------------------------------------------
// 总配置
// ---------------------------------------------------------------------------
struct Config {
    CaptureConfig capture;                 // 采集
    InferenceConfig inference;             // 推理
    PacerConfig pacer;                     // 稳帧
    EncodeConfig encode;                   // 编码
    StreamConfig stream;                   // 推流
    RecordConfig record;                   // 录制
    LogConfig log;                         // 日志
    uint32_t monitor_interval_ms = 5000;   // 监控日志间隔（毫秒）
};

// 从 YAML 配置文件加载配置（实现在 config.cpp）。
bool LoadConfigFromFile(const std::string& path, Config& config);

} // namespace vision
