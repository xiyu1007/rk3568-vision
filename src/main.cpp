// ============================================================================
// main.cpp — 程序入口（YAML 配置加载 + 信号处理 + 流水线生命周期）
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <type_traits>

#include <getopt.h>

#include "common.hpp"
#include "logger.hpp"
#include "pipeline.hpp"

namespace vision {

namespace {

// 去除字符串首尾空白。
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// 去掉字符串值两端的引号。
std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

// 精简 YAML 解析：把
//   section:
//     key: value
// 解析成 "section.key" -> "value" 的扁平 map（顶层键无前缀）。
std::map<std::string, std::string> parseYaml(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    if (!f.is_open()) return kv;

    std::string line, section;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');                      // 去注释
        if (hash != std::string::npos) line = line.substr(0, hash);

        std::string t = trim(line);
        if (t.empty()) continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(t.substr(0, colon));
        std::string val = unquote(trim(t.substr(colon + 1)));
        bool has_indent = (line[0] == ' ' || line[0] == '\t');

        if (val.empty()) {                                 // section
            if (!has_indent) section = key;
        } else {                                           // key: value
            if (has_indent && !section.empty()) kv[section + "." + key] = val;
            else kv[key] = val;
        }
    }
    return kv;
}

// 按 key 取值；不存在则保留原值（数值/字符串/bool 自动转换）。
template <typename T>
void tryGet(const std::map<std::string, std::string>& kv,
            const std::string& key, T& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    if constexpr (std::is_same_v<T, std::string>) {
        out = it->second;
    } else if constexpr (std::is_same_v<T, bool>) {
        out = (it->second == "true" || it->second == "1" || it->second == "yes");
    } else {
        std::istringstream iss(it->second);
        iss >> out;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 从 YAML 文件加载配置
// ---------------------------------------------------------------------------
bool loadConfig(const std::string& path, Config& cfg) {
    auto kv = parseYaml(path);
    if (kv.empty()) {
        LOG_WARN("config '%s' not found, use defaults", path.c_str());
        return false;
    }

    tryGet(kv, "capture.source", cfg.capture.source);
    tryGet(kv, "capture.device", cfg.capture.device);
    tryGet(kv, "capture.file",   cfg.capture.file);
    tryGet(kv, "capture.width",  cfg.capture.width);
    tryGet(kv, "capture.height", cfg.capture.height);
    tryGet(kv, "capture.fps",    cfg.capture.fps);
    tryGet(kv, "capture.buffer_count", cfg.capture.buffer_count);
    tryGet(kv, "capture.use_mplane",   cfg.capture.use_mplane);

    tryGet(kv, "inference.enabled",        cfg.inference.enabled);
    tryGet(kv, "inference.model_path",     cfg.inference.model_path);
    tryGet(kv, "inference.labels_path",    cfg.inference.labels_path);
    tryGet(kv, "inference.conf_threshold", cfg.inference.conf_threshold);
    tryGet(kv, "inference.nms_threshold",  cfg.inference.nms_threshold);
    tryGet(kv, "inference.model_width",    cfg.inference.model_width);
    tryGet(kv, "inference.model_height",   cfg.inference.model_height);
    tryGet(kv, "inference.npu_core",       cfg.inference.npu_core);
    tryGet(kv, "inference.use_sigmoid",    cfg.inference.use_sigmoid);

    tryGet(kv, "pacer.target_fps",      cfg.pacer.target_fps);
    tryGet(kv, "pacer.allow_duplicate", cfg.pacer.allow_duplicate);

    tryGet(kv, "encode.hardware", cfg.encode.hardware);
    tryGet(kv, "encode.bitrate",  cfg.encode.bitrate);
    tryGet(kv, "encode.gop_size", cfg.encode.gop_size);
    tryGet(kv, "encode.preset",   cfg.encode.preset);
    tryGet(kv, "encode.profile",  cfg.encode.profile);

    tryGet(kv, "stream.enabled",            cfg.stream.enabled);
    tryGet(kv, "stream.url",                cfg.stream.url);
    tryGet(kv, "stream.reconnect",          cfg.stream.reconnect);
    tryGet(kv, "stream.reconnect_delay_ms", cfg.stream.reconnect_delay_ms);
    tryGet(kv, "stream.max_reconnect",      cfg.stream.max_reconnect);

    tryGet(kv, "record.enabled", cfg.record.enabled);
    tryGet(kv, "record.path",    cfg.record.path);

    tryGet(kv, "logging.level",         cfg.log.level);
    tryGet(kv, "logging.file",          cfg.log.file);
    tryGet(kv, "logging.console",       cfg.log.console);
    tryGet(kv, "logging.async",         cfg.log.async);
    tryGet(kv, "logging.max_size",      cfg.log.max_size);
    tryGet(kv, "logging.backup_count",  cfg.log.backup_count);

    tryGet(kv, "monitor_interval_ms", cfg.monitor_interval_ms);

    LOG_INFO("config loaded from %s", path.c_str());
    return true;
}

} // namespace vision

namespace {

std::atomic<bool> g_stop{false};

void signalHandler(int sig) {
    (void)sig;
    g_stop.store(true);
}

void printUsage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  -c, --config PATH    config file (default: conf/default.yaml)\n"
        "  -d, --device DEV     V4L2 device\n"
        "  -W/-H/-f N           capture width/height/fps\n"
        "  -s, --stream URL     RTMP url\n"
        "      --no-stream      disable streaming\n"
        "      --no-inference   disable detection\n"
        "      --record PATH    enable MP4 recording to PATH\n"
        "  -v, --verbose        log level = debug\n"
        "  -h, --help           show help\n", prog);
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    std::string config_path = "conf/default.yaml";
    std::string device, stream_url, record_path;
    int width = 0, height = 0, fps = 0;
    bool no_stream = false, no_inference = false, verbose = false;

    static const option kLongOpts[] = {
        {"config", required_argument, nullptr, 'c'},
        {"device", required_argument, nullptr, 'd'},
        {"width",  required_argument, nullptr, 'W'},
        {"height", required_argument, nullptr, 'H'},
        {"fps",    required_argument, nullptr, 'f'},
        {"stream", required_argument, nullptr, 's'},
        {"no-stream",    no_argument, nullptr, 1000},
        {"no-inference", no_argument, nullptr, 1001},
        {"record",       required_argument, nullptr, 1002},
        {"verbose", no_argument, nullptr, 'v'},
        {"help",    no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:W:H:f:s:vh", kLongOpts, nullptr)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'd': device = optarg; break;
            case 'W': width = std::atoi(optarg); break;
            case 'H': height = std::atoi(optarg); break;
            case 'f': fps = std::atoi(optarg); break;
            case 's': stream_url = optarg; break;
            case 1000: no_stream = true; break;
            case 1001: no_inference = true; break;
            case 1002: record_path = optarg; break;
            case 'v': verbose = true; break;
            case 'h': printUsage(argv[0]); return 0;
            default: printUsage(argv[0]); return 1;
        }
    }

    // 加载配置 + 命令行覆盖。
    vision::Config cfg;
    vision::loadConfig(config_path, cfg);
    if (!device.empty())     cfg.capture.device = device;
    if (width > 0)           cfg.capture.width = width;
    if (height > 0)          cfg.capture.height = height;
    if (fps > 0)             cfg.capture.fps = fps;
    if (!stream_url.empty()) { cfg.stream.url = stream_url; cfg.stream.enabled = true; }
    if (no_stream)           cfg.stream.enabled = false;
    if (no_inference)        cfg.inference.enabled = false;
    if (!record_path.empty()) { cfg.record.path = record_path; cfg.record.enabled = true; }
    if (verbose)             cfg.log.level = "debug";

    vision::Logger::instance().init(cfg.log);

    vision::Pipeline pipeline(cfg);
    if (!pipeline.init()) { LOG_ERROR("pipeline init failed"); return 1; }
    if (!pipeline.start()) { LOG_ERROR("pipeline start failed"); return 1; }

    LOG_INFO("pipeline running, Ctrl+C to stop");
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    LOG_INFO("shutting down...");
    pipeline.stop();
    vision::Logger::instance().shutdown();
    return 0;
}
