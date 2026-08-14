// ============================================================================
// config.cpp — JSON 配置加载实现
// ============================================================================

#include "config.hpp"
#include "logger.hpp"

#include <fstream>
#include <string>

#include "json.hpp"   // nlohmann/json 单头库（third_lib/json/json.hpp）

namespace vision {

namespace {

// 从 json 对象按 key 取值；若 key 不存在或类型不符则保留原值（返回 false）。
template <typename T>
bool tryGet(const nlohmann::json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return false;
    out = it->get<T>();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 加载配置
// ---------------------------------------------------------------------------
bool loadConfig(const std::string& path, Config& cfg) {
    // 1. 读取 JSON 文件。
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOG_WARN("config file '%s' not found, use defaults", path.c_str());
        return false;
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        LOG_ERROR("config parse error: %s", e.what());
        return false;
    }

    // 2. 逐段读取（每个字段都用 tryGet 保留默认值）。
    if (j.contains("capture")) {
        const auto& s = j["capture"];
        tryGet(s, "device",        cfg.capture.device);
        tryGet(s, "width",         cfg.capture.width);
        tryGet(s, "height",        cfg.capture.height);
        tryGet(s, "fps",           cfg.capture.fps);
        tryGet(s, "pixel_format",  cfg.capture.pixel_format);
        tryGet(s, "buffer_count",  cfg.capture.buffer_count);
        tryGet(s, "use_mplane",    cfg.capture.use_mplane);
    }
    if (j.contains("inference")) {
        const auto& s = j["inference"];
        tryGet(s, "enabled",         cfg.inference.enabled);
        tryGet(s, "backend",         cfg.inference.backend);
        tryGet(s, "model_path",      cfg.inference.model_path);
        tryGet(s, "labels_path",     cfg.inference.labels_path);
        tryGet(s, "conf_threshold",  cfg.inference.conf_threshold);
        tryGet(s, "nms_threshold",   cfg.inference.nms_threshold);
        tryGet(s, "model_width",     cfg.inference.model_width);
        tryGet(s, "model_height",    cfg.inference.model_height);
        tryGet(s, "npu_core",        cfg.inference.npu_core);
    }
    if (j.contains("pacer")) {
        const auto& s = j["pacer"];
        tryGet(s, "enabled",          cfg.pacer.enabled);
        tryGet(s, "target_fps",       cfg.pacer.target_fps);
        tryGet(s, "allow_duplicate",  cfg.pacer.allow_duplicate);
        tryGet(s, "queue_depth",      cfg.pacer.queue_depth);
    }
    if (j.contains("encode")) {
        const auto& s = j["encode"];
        tryGet(s, "enabled",   cfg.encode.enabled);
        tryGet(s, "hardware",  cfg.encode.hardware);
        tryGet(s, "codec",     cfg.encode.codec);
        tryGet(s, "bitrate",   cfg.encode.bitrate);
        tryGet(s, "gop_size",  cfg.encode.gop_size);
        tryGet(s, "preset",    cfg.encode.preset);
        tryGet(s, "profile",   cfg.encode.profile);
    }
    if (j.contains("stream")) {
        const auto& s = j["stream"];
        tryGet(s, "enabled",            cfg.stream.enabled);
        tryGet(s, "url",                cfg.stream.url);
        tryGet(s, "reconnect",          cfg.stream.reconnect);
        tryGet(s, "reconnect_delay_ms", cfg.stream.reconnect_delay_ms);
        tryGet(s, "max_reconnect",      cfg.stream.max_reconnect);
    }
    if (j.contains("record")) {
        const auto& s = j["record"];
        tryGet(s, "enabled",          cfg.record.enabled);
        tryGet(s, "path",             cfg.record.path);
        tryGet(s, "segment_seconds",  cfg.record.segment_seconds);
    }
    if (j.contains("monitor")) {
        const auto& s = j["monitor"];
        tryGet(s, "enabled",          cfg.monitor.enabled);
        tryGet(s, "log_interval_ms",  cfg.monitor.log_interval_ms);
    }
    if (j.contains("logging")) {
        const auto& s = j["logging"];
        tryGet(s, "level",         cfg.log.level);
        tryGet(s, "file",          cfg.log.file);
        tryGet(s, "console",       cfg.log.console);
        tryGet(s, "async",         cfg.log.async);
        tryGet(s, "max_size",      cfg.log.max_size);
        tryGet(s, "backup_count",  cfg.log.backup_count);
    }

    LOG_INFO("config loaded from %s", path.c_str());
    return true;
}

} // namespace vision
