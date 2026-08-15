// ============================================================================
// main.cpp — 程序入口（信号处理 + 配置加载 + 流水线生命周期）
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <getopt.h>

#include "vision/config.hpp"
#include "vision/logger.hpp"
#include "vision/pipeline.hpp"

namespace {

// 全局停止标志（信号处理设置，主循环轮询）。
std::atomic<bool> g_stop{false};

// 信号处理：SIGINT/SIGTERM 时设置停止标志，触发优雅退出。
void SignalHandler(int signal) {
    (void)signal;
    g_stop.store(true);
}

// 打印命令行用法。
void PrintUsage(const char* program) {
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
        "  -h, --help           show help\n",
        program);
}

} // namespace

int main(int argc, char* argv[]) {
    // 信号处理：SIGINT/SIGTERM 优雅退出，SIGPIPE 忽略（避免 RTMP 断开导致进程退出）。
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    std::string config_path = "conf/default.yaml";
    std::string device;
    std::string stream_url;
    std::string record_path;
    int width = 0, height = 0, fps = 0;
    bool no_stream = false, no_inference = false, verbose = false;

    static const option kLongOptions[] = {
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

    int option;
    while ((option = getopt_long(argc, argv, "c:d:W:H:f:s:vh",
                                 kLongOptions, nullptr)) != -1) {
        switch (option) {
            case 'c':   config_path = optarg; break;
            case 'd':   device = optarg; break;
            case 'W':   width = std::atoi(optarg); break;
            case 'H':   height = std::atoi(optarg); break;
            case 'f':   fps = std::atoi(optarg); break;
            case 's':   stream_url = optarg; break;
            case 1000:  no_stream = true; break;
            case 1001:  no_inference = true; break;
            case 1002:  record_path = optarg; break;
            case 'v':   verbose = true; break;
            case 'h':   PrintUsage(argv[0]); return 0;
            default:    PrintUsage(argv[0]); return 1;
        }
    }

    // 加载配置 + 命令行覆盖。
    vision::Config config;
    vision::LoadConfigFromFile(config_path, config);
    if (!device.empty())        { config.capture.device = device; }
    if (width > 0)             { config.capture.width = static_cast<uint32_t>(width); }
    if (height > 0)            { config.capture.height = static_cast<uint32_t>(height); }
    if (fps > 0)               { config.capture.fps = static_cast<uint32_t>(fps); }
    if (!stream_url.empty())   { config.stream.url = stream_url; config.stream.enabled = true; }
    if (no_stream)             { config.stream.enabled = false; }
    if (no_inference)          { config.inference.enabled = false; }
    if (!record_path.empty())  { config.record.path = record_path; config.record.enabled = true; }
    if (verbose)               { config.log.level = "debug"; }
#ifdef VISION_DEBUG
    // DEBUG 构建强制 debug 日志级别，输出性能分析/队列深度/帧生命周期等详细信息。
    config.log.level = "debug";
#endif

    vision::Logger::instance().init(config.log);

    vision::Pipeline pipeline(config);
    if (!pipeline.Initialize()) {
        vision::Logger::instance().error("pipeline init failed");
        return 1;
    }
    if (!pipeline.Start()) {
        vision::Logger::instance().error("pipeline start failed");
        return 1;
    }

    vision::Logger::instance().info("pipeline running, Ctrl+C to stop");
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    vision::Logger::instance().info("shutting down...");
    pipeline.Stop();
    vision::Logger::instance().shutdown();
    return 0;
}
