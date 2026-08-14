// ============================================================================
// main.cpp — 程序入口
// ============================================================================
//
// 启动流程：
//   1. 注册信号处理（SIGINT/SIGTERM 优雅退出，SIGPIPE 忽略）
//   2. 解析命令行参数
//   3. 加载配置（JSON 文件 + 命令行覆盖）
//   4. 初始化日志系统
//   5. 创建并初始化流水线
//   6. 启动流水线与监控
//   7. 主循环等待退出信号
//   8. 停止流水线、监控、日志并退出
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <getopt.h>

#include "config.hpp"
#include "logger.hpp"
#include "monitor.hpp"
#include "pipeline.hpp"

namespace {

// 全局退出标志：信号处理函数里只做这一件简单的事（信号安全）。
std::atomic<bool> g_stop{false};

// 信号处理函数。
void signalHandler(int sig) {
    if (sig == SIGPIPE) return;   // SIGPIPE 已在 main 里忽略，这里兜底
    g_stop.store(true);
}

// 打印使用说明。
void printUsage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  -c, --config PATH   config file (default: conf/default.json)\n"
        "  -d, --device DEV    V4L2 device (e.g. /dev/video0)\n"
        "  -W, --width  N      capture width\n"
        "  -H, --height N      capture height\n"
        "  -f, --fps    N      capture fps\n"
        "  -s, --stream URL    RTMP push url\n"
        "      --no-stream     disable RTMP streaming\n"
        "      --no-inference  disable detection\n"
        "      --record PATH   enable MP4 recording to PATH\n"
        "  -v, --verbose       log level = debug\n"
        "  -h, --help          show this help\n",
        prog);
}

} // namespace

int main(int argc, char* argv[]) {
    // ---- 1. 信号处理 ----
    std::signal(SIGINT,  signalHandler);   // Ctrl+C
    std::signal(SIGTERM, signalHandler);   // kill / systemd stop
    std::signal(SIGPIPE, SIG_IGN);         // RTMP 断连产生 SIGPIPE，忽略避免崩溃

    // ---- 2. 命令行解析 ----
    std::string config_path = "conf/default.json";
    std::string device, stream_url, record_path;
    int width = 0, height = 0, fps = 0;
    bool no_stream = false, no_inference = false, verbose = false;

    static const option kLongOpts[] = {
        {"config",       required_argument, nullptr, 'c'},
        {"device",       required_argument, nullptr, 'd'},
        {"width",        required_argument, nullptr, 'W'},
        {"height",       required_argument, nullptr, 'H'},
        {"fps",          required_argument, nullptr, 'f'},
        {"stream",       required_argument, nullptr, 's'},
        {"no-stream",    no_argument,       nullptr, 1000},
        {"no-inference", no_argument,       nullptr, 1001},
        {"record",       required_argument, nullptr, 1002},
        {"verbose",      no_argument,       nullptr, 'v'},
        {"help",         no_argument,       nullptr, 'h'},
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
            default:  printUsage(argv[0]); return 1;
        }
    }

    // ---- 3. 加载配置 ----
    vision::Config cfg;
    if (!vision::loadConfig(config_path, cfg)) {
        std::printf("[WARN] use default config\n");
    }
    // 命令行覆盖配置。
    if (!device.empty())     cfg.capture.device = device;
    if (width > 0)           cfg.capture.width  = static_cast<uint32_t>(width);
    if (height > 0)          cfg.capture.height = static_cast<uint32_t>(height);
    if (fps > 0)             cfg.capture.fps    = static_cast<uint32_t>(fps);
    if (!stream_url.empty()) { cfg.stream.url = stream_url; cfg.stream.enabled = true; }
    if (no_stream)           cfg.stream.enabled = false;
    if (no_inference)        cfg.inference.enabled = false;
    if (!record_path.empty()) { cfg.record.path = record_path; cfg.record.enabled = true; }
    if (verbose)             cfg.log.level = "debug";

    // ---- 4. 初始化日志 ----
    vision::Logger::instance().init(cfg.log);

    // ---- 5. 创建并初始化流水线 ----
    vision::Pipeline pipeline(cfg);
    if (!pipeline.init()) {
        LOG_ERROR("pipeline init failed");
        vision::Logger::instance().shutdown();
        return 1;
    }

    // ---- 6. 启动流水线与监控 ----
    if (!pipeline.start()) {
        LOG_ERROR("pipeline start failed");
        vision::Logger::instance().shutdown();
        return 1;
    }
    if (cfg.monitor.enabled) {
        vision::Monitor::instance().start(cfg.monitor);
    }

    LOG_INFO("vision pipeline running, press Ctrl+C to stop");

    // ---- 7. 主循环等待退出 ----
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ---- 8. 停止与清理 ----
    LOG_INFO("shutting down...");
    pipeline.stop();
    vision::Monitor::instance().stop();
    vision::Logger::instance().shutdown();
    return 0;
}
