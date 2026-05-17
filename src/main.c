#include "types.h"
#include "config.h"
#include "logger.h"
#include "sig.h"
#include "monitor.h"
#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  -c FILE     Config file (default: config/default.yaml)\n"
        "  -d DEV      V4L2 device (default: /dev/video0)\n"
        "  -W W        Width (default: 1920)\n"
        "  -H H        Height (default: 1080)\n"
        "  -f FPS      FPS (default: 30)\n"
        "  -s URL      RTMP stream URL\n"
        "  -n          Disable inference\n"
        "  -N          Disable display\n"
        "  -v          Verbose logging\n"
        "  -h          Help\n",
        prog);
}

int main(int argc, char* argv[]) {
    sig_setup();

    const char* config_path = "config/default.yaml";
    app_cfg_t   cfg;

    struct option long_opts[] = {
        {"config", required_argument, NULL, 'c'},
        {"device", required_argument, NULL, 'd'},
        {"width",  required_argument, NULL, 'W'},
        {"height", required_argument, NULL, 'H'},
        {"fps",    required_argument, NULL, 'f'},
        {"stream", required_argument, NULL, 's'},
        {"no-inference", no_argument, NULL, 'n'},
        {"no-display",   no_argument, NULL, 'N'},
        {"verbose",      no_argument, NULL, 'v'},
        {"help",         no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    config_load(config_path, &cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:W:H:f:s:nNvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            config_load(config_path, &cfg);
            break;
        case 'd': strncpy(cfg.cap.device, optarg, 63); break;
        case 'W': cfg.cap.width  = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'H': cfg.cap.height = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'f': cfg.cap.fps    = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 's': strncpy(cfg.strm.url, optarg, 255); cfg.strm.enabled = 1; break;
        case 'n': cfg.inf.enabled = 0; break;
        case 'N': cfg.disp.enabled = 0; break;
        case 'v': break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    logger_init("log/rk3568_vision.log", LOG_LEVEL_INFO, 1, 1);

    LOG_INFO("RK3568 Vision Pipeline v3.0.0");
    LOG_INFO("capture:  %s %ux%u@%u %s",
             cfg.cap.device, cfg.cap.width, cfg.cap.height,
             cfg.cap.fps, cfg.cap.pixfmt);
    LOG_INFO("inference: %s", cfg.inf.enabled ? "enabled" : "disabled");
    LOG_INFO("stream:    %s", cfg.strm.enabled ? cfg.strm.url : "disabled");
    LOG_INFO("display:   %s", cfg.disp.enabled ? "enabled" : "disabled");

    pipeline_t* p = pipeline_create(&cfg);
    if (!p) {
        LOG_FATAL("pipeline_create failed");
        logger_shutdown();
        return 1;
    }

    if (!pipeline_start(p)) {
        LOG_FATAL("pipeline_start failed");
        pipeline_stop(p);
        logger_shutdown();
        return 1;
    }

    if (cfg.mon.enabled) monitor_start();

    LOG_INFO("Running... Ctrl+C to stop");
    while (pipeline_running(p) && !sig_shutdown())
        sleep(1);

    LOG_INFO("Shutting down...");
    sig_request_shutdown();
    pipeline_stop(p);
    monitor_stop();
    logger_shutdown();

    return 0;
}
