#pragma once

#include "types.hpp"

#include <string>
#include <map>
#include <vector>
#include <memory>

namespace rk3568_vision {

// ============================================================================
// 统一配置系统 — 基于 YAML 的最小实现（零外部依赖）
//
// 设计原因:
// - 不引入 yaml-cpp 等重量级库，减少交叉编译复杂度
// - 嵌入式场景配置项有限（~50个），手写解析器足够
// - 支持嵌套键：capture.width, inference.conf_threshold
// - 支持运行时热更新（通过 reload() 重新读取）
// ============================================================================
class Config {
public:
    static Config& instance();

    void load(const std::string& yaml_path);
    void reload();

    // 类型安全的获取接口
    std::string get_string(const std::string& key, const std::string& def = "") const;
    int64_t    get_int(const std::string& key, int64_t def = 0) const;
    double     get_double(const std::string& key, double def = 0.0) const;
    bool       get_bool(const std::string& key, bool def = false) const;

    const std::string& file_path() const { return file_path_; }

private:
    Config() = default;

    void parse_line(const std::string& line, std::string& current_section,
                    int& indent_level);

    std::map<std::string, std::string> values_;
    std::string file_path_;
};

// ============================================================================
// Pipeline 配置结构体（从 Config 填充，避免运行时查表开销）
// ============================================================================
struct CaptureConfig {
    std::string device       = "/dev/video0";
    uint32_t    width        = 1920;
    uint32_t    height       = 1080;
    uint32_t    fps          = 30;
    std::string pixel_format = "NV12";
    uint32_t    buffer_count = 6;
    std::string buffer_type  = "MPLANE";
};

struct InferenceConfig {
    bool        enabled        = true;
    std::string model_path     = "model/yolov5s.rknn";
    std::string labels_path    = "model/coco_80_labels_list.txt";
    float       conf_threshold = 0.25f;
    float       nms_threshold  = 0.45f;
    uint32_t    model_width    = 640;
    uint32_t    model_height   = 640;
    bool        quantized      = true;
    uint32_t    npu_core       = 0;
};

struct EncodeConfig {
    bool        enabled      = true;
    std::string codec        = "h264";
    uint32_t    bitrate      = 4000000;
    uint32_t    gop_size     = 60;
    std::string preset       = "fast";
    std::string profile      = "high";
    // 分辨率/FPS使用capture配置，不独立设置
};

struct StreamConfig {
    bool        enabled         = false;
    std::string url             = "rtmp://127.0.0.1/live/stream";
    bool        reconnect       = true;
    uint32_t    reconnect_delay = 2000;
    int32_t     max_reconnect   = 10;
};

struct DisplayConfig {
    bool        enabled     = true;
    std::string window_name = "RK3568 Vision";
    bool        show_fps    = true;
    bool        show_osd    = true;
};

struct MonitorConfig {
    bool     enabled      = true;
    uint32_t log_interval = 5000;
};

void load_configs(CaptureConfig& cap, InferenceConfig& inf, EncodeConfig& enc,
                  StreamConfig& strm, DisplayConfig& disp, MonitorConfig& mon);

} // namespace rk3568_vision
