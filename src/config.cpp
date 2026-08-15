// ============================================================================
// config.cpp — 配置加载（自研精简 YAML 解析器，零第三方依赖）
// ============================================================================
//
// 把 conf/default.yaml 解析成扁平 map（"section.key" -> "value"），
// 再填充到 Config 结构体。支持命令行参数覆盖（在 main.cpp 里处理）。
// ============================================================================

#include "vision/config.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <type_traits>

#include "vision/logger.hpp"

namespace vision {

namespace {

// 去除字符串首尾空白字符。
std::string TrimWhitespace(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

// 去掉字符串值两端引号（单引号或双引号）。
std::string RemoveQuotes(const std::string& text) {
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

// 精简 YAML 解析：把
//   section:
//     key: value
// 解析成扁平 map："section.key" -> "value"（顶层键无前缀）。
std::map<std::string, std::string> ParseYaml(const std::string& path) {
    std::map<std::string, std::string> key_values;
    std::ifstream file(path);
    if (!file.is_open()) {
        return key_values;
    }

    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        // 去掉行尾注释（'#' 开头到行尾）。
        const size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        const std::string trimmed = TrimWhitespace(line);
        if (trimmed.empty()) {
            continue;
        }
        const size_t colon_pos = trimmed.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        const std::string key = TrimWhitespace(trimmed.substr(0, colon_pos));
        const std::string value = RemoveQuotes(TrimWhitespace(trimmed.substr(colon_pos + 1)));
        const bool has_indent = (line[0] == ' ' || line[0] == '\t');

        if (value.empty()) {
            // 空值表示这是一个 section 头。
            if (!has_indent) {
                section = key;
            }
        } else {
            // 有缩进的键值对归属到当前 section。
            if (has_indent && !section.empty()) {
                key_values[section + "." + key] = value;
            } else {
                key_values[key] = value;
            }
        }
    }
    return key_values;
}

// 按 key 从 map 取值；key 不存在则保留 out 的默认值。
// 支持 string / bool / 数值类型的自动转换。
template <typename T>
void TryGetValue(const std::map<std::string, std::string>& key_values,
                 const std::string& key, T& out) {
    const auto it = key_values.find(key);
    if (it == key_values.end()) {
        return;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        out = it->second;
    } else if constexpr (std::is_same_v<T, bool>) {
        out = (it->second == "true" || it->second == "1" || it->second == "yes");
    } else {
        std::istringstream stream(it->second);
        stream >> out;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 从 YAML 文件加载配置
// ---------------------------------------------------------------------------
bool LoadConfigFromFile(const std::string& path, Config& config) {
    const auto key_values = ParseYaml(path);
    if (key_values.empty()) {
        Logger::instance().warn("config '%s' not found, use defaults", path.c_str());
        return false;
    }

    // ---- 采集 ----
    TryGetValue(key_values, "capture.source",          config.capture.source);
    TryGetValue(key_values, "capture.device",          config.capture.device);
    TryGetValue(key_values, "capture.file",            config.capture.file);
    TryGetValue(key_values, "capture.width",           config.capture.width);
    TryGetValue(key_values, "capture.height",          config.capture.height);
    TryGetValue(key_values, "capture.fps",             config.capture.fps);
    TryGetValue(key_values, "capture.buffer_count",    config.capture.buffer_count);
    TryGetValue(key_values, "capture.use_multi_planar", config.capture.use_multi_planar);

    // ---- 推理 ----
    TryGetValue(key_values, "inference.enabled",              config.inference.enabled);
    TryGetValue(key_values, "inference.model_path",           config.inference.model_path);
    TryGetValue(key_values, "inference.labels_path",          config.inference.labels_path);
    TryGetValue(key_values, "inference.confidence_threshold", config.inference.confidence_threshold);
    TryGetValue(key_values, "inference.nms_threshold",        config.inference.nms_threshold);
    TryGetValue(key_values, "inference.model_width",          config.inference.model_width);
    TryGetValue(key_values, "inference.model_height",         config.inference.model_height);
    TryGetValue(key_values, "inference.npu_core",             config.inference.npu_core);
    TryGetValue(key_values, "inference.use_sigmoid",          config.inference.use_sigmoid);

    // ---- 稳帧 ----
    TryGetValue(key_values, "pacer.target_fps",      config.pacer.target_fps);
    TryGetValue(key_values, "pacer.allow_duplicate", config.pacer.allow_duplicate);

    // ---- 编码 ----
    TryGetValue(key_values, "encode.hardware", config.encode.hardware);
    TryGetValue(key_values, "encode.bitrate",  config.encode.bitrate);
    TryGetValue(key_values, "encode.gop_size", config.encode.gop_size);
    TryGetValue(key_values, "encode.preset",   config.encode.preset);
    TryGetValue(key_values, "encode.profile",  config.encode.profile);

    // ---- 推流 ----
    TryGetValue(key_values, "stream.enabled",            config.stream.enabled);
    TryGetValue(key_values, "stream.url",                config.stream.url);
    TryGetValue(key_values, "stream.reconnect",          config.stream.reconnect);
    TryGetValue(key_values, "stream.reconnect_delay_ms", config.stream.reconnect_delay_ms);
    TryGetValue(key_values, "stream.max_reconnect",      config.stream.max_reconnect);

    // ---- 录制 ----
    TryGetValue(key_values, "record.enabled", config.record.enabled);
    TryGetValue(key_values, "record.path",    config.record.path);

    // ---- 日志 ----
    TryGetValue(key_values, "logging.level",        config.log.level);
    TryGetValue(key_values, "logging.file",         config.log.file);
    TryGetValue(key_values, "logging.console",      config.log.console);
    TryGetValue(key_values, "logging.async",        config.log.async);
    TryGetValue(key_values, "logging.max_size",     config.log.max_size);
    TryGetValue(key_values, "logging.backup_count", config.log.backup_count);

    // ---- 监控 ----
    TryGetValue(key_values, "monitor_interval_ms", config.monitor_interval_ms);

    Logger::instance().info("config loaded from %s", path.c_str());
    return true;
}

} // namespace vision
