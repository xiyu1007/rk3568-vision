#include "config.hpp"
#include "logger.hpp"
#include "types.hpp"
#include "pipeline.hpp"
#include "sig_handler.h"
#include "perf.hpp"
#include "monitor.hpp"

#include <cstdlib>
#include <cstdio>
#include <getopt.h>

using namespace rk3568_vision;

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  -c, --config FILE    Configuration file (default: config/default.yaml)\n"
        "  -d, --device DEV     V4L2 device (default: /dev/video0)\n"
        "  -W, --width W        Capture width (default: 1920)\n"
        "  -H, --height H       Capture height (default: 1080)\n"
        "  -f, --fps FPS        Capture FPS (default: 30)\n"
        "  -s, --stream URL     RTMP stream URL (default: none)\n"
        "  -n, --no-inference   Disable AI inference\n"
        "  -N, --no-display     Disable local display\n"
        "  -v, --verbose        Verbose logging\n"
        "  -h, --help           Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -d /dev/video0                               Local display only\n"
        "  %s -c config/production.yaml                     Use production config\n"
        "  %s -s rtmp://192.168.1.100/live/stream           Stream to RTMP\n"
        "\n",
        prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    signal_setup_handlers();

    // 默认配置路径
    std::string config_path = "config/default.yaml";
    std::string rtmp_url;
    bool no_inference = false;
    bool no_display   = false;

    struct option long_opts[] = {
        {"config",       required_argument, nullptr, 'c'},
        {"device",       required_argument, nullptr, 'd'},
        {"width",        required_argument, nullptr, 'W'},
        {"height",       required_argument, nullptr, 'H'},
        {"fps",          required_argument, nullptr, 'f'},
        {"stream",       required_argument, nullptr, 's'},
        {"no-inference", no_argument,       nullptr, 'n'},
        {"no-display",   no_argument,       nullptr, 'N'},
        {"verbose",      no_argument,       nullptr, 'v'},
        {"help",         no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    CaptureConfig   cap_cfg;
    InferenceConfig inf_cfg;
    EncodeConfig    enc_cfg;
    StreamConfig    strm_cfg;
    DisplayConfig   disp_cfg;
    MonitorConfig   mon_cfg;

    // 加载 YAML 配置
    Config::instance().load(config_path);
    load_configs(cap_cfg, inf_cfg, enc_cfg, strm_cfg, disp_cfg, mon_cfg);

    // 解析命令行参数（可覆盖 YAML 配置）
    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:W:H:f:s:nNvh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'c': config_path  = optarg; Config::instance().load(config_path); load_configs(cap_cfg, inf_cfg, enc_cfg, strm_cfg, disp_cfg, mon_cfg); break;
            case 'd': cap_cfg.device    = optarg; break;
            case 'W': cap_cfg.width     = strtoul(optarg, nullptr, 10); break;
            case 'H': cap_cfg.height    = strtoul(optarg, nullptr, 10); break;
            case 'f': cap_cfg.fps       = strtoul(optarg, nullptr, 10); break;
            case 's': rtmp_url          = optarg; strm_cfg.enabled = true; strm_cfg.url = optarg; break;
            case 'n': no_inference = true; break;
            case 'N': no_display   = true; break;
            case 'v': break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    if (no_inference) inf_cfg.enabled = false;
    if (no_display)   disp_cfg.enabled = false;

    // 初始化日志
    Logger::instance().init("log/rk3568_vision.log", LogLevel::INFO, true, true);

    LOG_INFO("RK3568 Vision Pipeline v2.0.0");
    LOG_INFO("Capture:  %s %ux%u@%u %s", cap_cfg.device.c_str(),
             cap_cfg.width, cap_cfg.height, cap_cfg.fps, cap_cfg.pixel_format.c_str());
    LOG_INFO("Inference: %s", inf_cfg.enabled ? "enabled" : "disabled");
    LOG_INFO("Stream:   %s", strm_cfg.enabled ? strm_cfg.url.c_str() : "disabled");
    LOG_INFO("Display:  %s", disp_cfg.enabled ? "enabled" : "disabled");

    // 创建并启动 Pipeline
    Pipeline pipeline;
    if (!pipeline.init(cap_cfg, inf_cfg, enc_cfg, strm_cfg, disp_cfg)) {
        LOG_FATAL("Pipeline initialization failed");
        return 1;
    }

    if (!pipeline.start()) {
        LOG_FATAL("Pipeline start failed");
        return 1;
    }

    // 系统监控
    SystemMonitor monitor;
    if (mon_cfg.enabled) monitor.start();

    // 等待关闭信号
    LOG_INFO("Running... Press Ctrl+C to stop");
    while (pipeline.is_running() && !signal_is_shutdown()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_INFO("Shutting down...");
    signal_request_shutdown();
    pipeline.stop();
    monitor.stop();

    Logger::instance().shutdown();
    return 0;
}
