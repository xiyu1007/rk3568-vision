#include "config.hpp"
#include "logger.hpp"

#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cctype>

namespace rk3568_vision {

Config& Config::instance() {
    static Config inst;
    return inst;
}

void Config::parse_line(const std::string& line, std::string& current_section,
                        int& indent_level) {
    if (line.empty() || line[0] == '#') return;

    size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') ++pos;
    int new_indent = static_cast<int>(pos);

    std::string content = line.substr(pos);
    if (content.empty()) return;

    if (content.back() == ':') {
        std::string section = content.substr(0, content.size() - 1);
        if (new_indent == 0) {
            current_section = section;
        } else if (!current_section.empty()) {
            current_section += "." + section;
        } else {
            current_section = section;
        }
        indent_level = new_indent;
        return;
    }

    size_t colon = content.find(':');
    if (colon == std::string::npos) return;

    std::string key = content.substr(0, colon);
    key.erase(0, key.find_first_not_of(" "));
    key.erase(key.find_last_not_of(" ") + 1);

    std::string val = content.substr(colon + 1);
    size_t vstart = val.find_first_not_of(" ");
    if (vstart != std::string::npos) val = val.substr(vstart);
    size_t comment_pos = val.find(" #");
    if (comment_pos != std::string::npos) val = val.substr(0, comment_pos);
    val.erase(val.find_last_not_of(" \t\r\n") + 1);

    std::string full_key = current_section.empty()
        ? key : current_section + "." + key;
    values_[full_key] = val;
}

void Config::load(const std::string& yaml_path) {
    file_path_ = yaml_path;
    values_.clear();

    std::ifstream file(yaml_path);
    if (!file.is_open()) {
        LOG_WARN("Config file not found: %s, using defaults", yaml_path.c_str());
        return;
    }

    std::string current_section;
    int indent_level = 0;
    std::string line;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        parse_line(line, current_section, indent_level);
    }

    LOG_INFO("Config loaded: %s (%zu entries)", yaml_path.c_str(), values_.size());
}

void Config::reload() {
    if (!file_path_.empty()) load(file_path_);
}

std::string Config::get_string(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return (it != values_.end()) ? it->second : def;
}

int64_t Config::get_int(const std::string& key, int64_t def) const {
    auto it = values_.find(key);
    if (it != values_.end()) {
        char* end = nullptr;
        int64_t val = strtoll(it->second.c_str(), &end, 10);
        if (end != it->second.c_str()) return val;
    }
    return def;
}

double Config::get_double(const std::string& key, double def) const {
    auto it = values_.find(key);
    if (it != values_.end()) {
        char* end = nullptr;
        double val = strtod(it->second.c_str(), &end);
        if (end != it->second.c_str()) return val;
    }
    return def;
}

bool Config::get_bool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it != values_.end()) {
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v == "true" || v == "yes" || v == "1" || v == "on")  return true;
        if (v == "false" || v == "no" || v == "0" || v == "off") return false;
    }
    return def;
}

void load_configs(CaptureConfig& cap, InferenceConfig& inf, EncodeConfig& enc,
                  StreamConfig& strm, DisplayConfig& disp, MonitorConfig& mon) {
    auto& cfg = Config::instance();

    cap.device       = cfg.get_string("capture.device",        "/dev/video0");
    cap.width        = static_cast<uint32_t>(cfg.get_int("capture.width",    1920));
    cap.height       = static_cast<uint32_t>(cfg.get_int("capture.height",   1080));
    cap.fps          = static_cast<uint32_t>(cfg.get_int("capture.fps",      30));
    cap.pixel_format = cfg.get_string("capture.pixel_format",  "NV12");
    cap.buffer_count = static_cast<uint32_t>(cfg.get_int("capture.buffer_count", 6));
    cap.buffer_type  = cfg.get_string("capture.buffer_type",   "MPLANE");

    inf.enabled        = cfg.get_bool("inference.enabled",          true);
    inf.model_path     = cfg.get_string("inference.model_path",     "model/yolov5s.rknn");
    inf.labels_path    = cfg.get_string("inference.labels_path",    "model/coco_80_labels_list.txt");
    inf.conf_threshold = static_cast<float>(cfg.get_double("inference.conf_threshold", 0.25));
    inf.nms_threshold  = static_cast<float>(cfg.get_double("inference.nms_threshold",  0.45));
    inf.model_width    = static_cast<uint32_t>(cfg.get_int("inference.model_width",     640));
    inf.model_height   = static_cast<uint32_t>(cfg.get_int("inference.model_height",    640));
    inf.quantized      = cfg.get_bool("inference.quantized",        true);
    inf.npu_core       = static_cast<uint32_t>(cfg.get_int("inference.npu_core",        0));

    enc.enabled      = cfg.get_bool("encode.enabled",       true);
    enc.codec        = cfg.get_string("encode.codec",        "h264");
    enc.bitrate      = static_cast<uint32_t>(cfg.get_int("encode.bitrate",     4000000));
    enc.gop_size     = static_cast<uint32_t>(cfg.get_int("encode.gop_size",    60));
    enc.preset       = cfg.get_string("encode.preset",       "fast");
    enc.profile      = cfg.get_string("encode.profile",      "high");

    strm.enabled         = cfg.get_bool("stream.enabled",           false);
    strm.url             = cfg.get_string("stream.url",              "rtmp://127.0.0.1/live/stream");
    strm.reconnect       = cfg.get_bool("stream.reconnect",         true);
    strm.reconnect_delay = static_cast<uint32_t>(cfg.get_int("stream.reconnect_delay",  2000));
    strm.max_reconnect   = static_cast<int32_t>(cfg.get_int("stream.max_reconnect",     10));

    disp.enabled     = cfg.get_bool("display.enabled",       true);
    disp.window_name = cfg.get_string("display.window_name",  "RK3568 Vision");
    disp.show_fps    = cfg.get_bool("display.show_fps",      true);
    disp.show_osd    = cfg.get_bool("display.show_osd",      true);

    mon.enabled      = cfg.get_bool("monitor.enabled",        true);
    mon.log_interval = static_cast<uint32_t>(cfg.get_int("monitor.log_interval",  5000));
}

} // namespace rk3568_vision
