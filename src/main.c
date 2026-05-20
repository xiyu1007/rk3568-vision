/*
 * ==========================================================================
 * main.c — 程序入口点
 * ==========================================================================
 *
 * **main() 函数职责**：
 *   1. 注册信号处理器（Ctrl+C/SIGTERM 优雅退出）
 *   2. 解析命令行参数（可覆盖 YAML 配置文件中的值）
 *   3. 加载 YAML 配置文件
 *   4. 初始化日志系统
 *   5. 创建流水线（pipeline_create）
 *   6. 启动流水线（pipeline_start）
 *   7. 主循环等待退出信号
 *   8. 清理资源（pipeline_stop, monitor_stop, logger_shutdown）
 *
 * **运行时序**：
 *   main()
 *   ├── sig_setup()                   注册信号处理
 *   ├── config_load(...)              加载 YAML 配置
 *   ├── 命令行参数解析                 覆盖配置
 *   ├── logger_init(...)              启动日志系统
 *   ├── pipeline_create(&cfg)         创建流水线
 *   │   ├── V4L2 打开设备
 *   │   ├── RKNN 加载模型
 *   │   ├── FFmpeg 初始化编码器
 *   │   ├── RTMP 建立连接（如果启用）
 *   │   └── OpenCV 创建显示窗口（如果启用）
 *   ├── pipeline_start(p)             启动流水线
 *   │   ├── V4L2 STREAMON
 *   │   └── 创建 4 个工作线程
 *   ├── monitor_start()               启动性能监控
 *   ├── while(running && !sig_shutdown)  [主循环等待]
 *   │   └── sleep(1)                  每秒检查一次退出条件
 *   └── 清理（stop/shutdown/close）
 */

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

/* 打印帮助信息 */
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

/*
 * 程序主入口
 *
 * 返回值：
 *   0：正常退出
 *   1：初始化失败（pipeline_create 或 pipeline_start 失败）
 */
int main(int argc, char* argv[]) {
    /*
     * 第一步：尽早注册信号处理
     * 确保任何时刻的 Ctrl+C 都能被捕获
     */
    sig_setup();

    const char* config_path = "config/default.yaml";
    app_cfg_t   cfg;

    /* 定义长选项（--config, --device 等） */
    struct option long_opts[] = {
        {"config",       required_argument, NULL, 'c'},
        {"device",       required_argument, NULL, 'd'},
        {"width",        required_argument, NULL, 'W'},
        {"height",       required_argument, NULL, 'H'},
        {"fps",          required_argument, NULL, 'f'},
        {"stream",       required_argument, NULL, 's'},
        {"no-inference", no_argument,       NULL, 'n'},
        {"no-display",   no_argument,       NULL, 'N'},
        {"verbose",      no_argument,       NULL, 'v'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    /*
     * 先加载默认配置文件
     * 命令行参数可以覆盖（但 -c 参数会重新加载）
     */
    config_load(config_path, &cfg);

    /*
     * 解析命令行参数
     * getopt_long 按顺序处理所有选项
     * 每次循环处理一个选项，覆盖对应的配置项
     *
     * 重要：-c 参数会重新调用 config_load
     * 所以如果同时指定 -c 和其他选项，-c 应该先被处理
     * （getopt_long 按命令行中出现的顺序处理）
     */
    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:W:H:f:s:nNvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            config_load(config_path, &cfg);  /* 重新加载指定的配置文件 */
            break;
        case 'd': strncpy(cfg.cap.device, optarg, 63); break;
        case 'W': cfg.cap.width  = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'H': cfg.cap.height = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'f': cfg.cap.fps    = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 's': strncpy(cfg.strm.url, optarg, 255); cfg.strm.enabled = 1; break;
        case 'n': cfg.inf.enabled = 0; break;
        case 'N': cfg.disp.enabled = 0; break;
        case 'v': break;  /* verbose 当前未实现额外逻辑 */
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /*
     * 初始化日志系统
     * 参数：日志文件路径, 最低输出级别, 输出到控制台, 异步写入
     * 必须在任何 LOG_* 调用之前初始化
     */
    logger_init("log/rk3568_vision.log", LOG_LEVEL_INFO, 1, 1);

    LOG_INFO("RK3568 Vision Pipeline v3.0.0");
    LOG_INFO("capture:  %s %ux%u@%u %s",
             cfg.cap.device, cfg.cap.width, cfg.cap.height,
             cfg.cap.fps, cfg.cap.pixfmt);
    LOG_INFO("inference: %s", cfg.inf.enabled ? "enabled" : "disabled");
    LOG_INFO("stream:    %s", cfg.strm.enabled ? cfg.strm.url : "disabled");
    LOG_INFO("display:   %s", cfg.disp.enabled ? "enabled" : "disabled");

    /* 创建流水线（初始化所有子模块） */
    pipeline_t* p = pipeline_create(&cfg);
    if (!p) {
        LOG_FATAL("pipeline_create failed");
        logger_shutdown();
        return 1;
    }

    /* 启动流水线（开始采集、推理、编码、显示） */
    if (!pipeline_start(p)) {
        LOG_FATAL("pipeline_start failed");
        pipeline_stop(p);
        logger_shutdown();
        return 1;
    }

    /* 启动性能监控线程（CPU/内存/温度 每 2 秒采样一次） */
    if (cfg.mon.enabled) monitor_start();

    /*
     * 主循环：等待退出信号
     *
     * pipeline_running(p)：检查流水线是否仍在运行
     * sig_shutdown()：检查是否收到 SIGINT/SIGTERM
     *
     * sleep(1)：每秒检查一次，节省 CPU
     * 一旦任一条件触发（错误或信号），退出循环进入清理
     */
    LOG_INFO("Running... Ctrl+C to stop");
    while (pipeline_running(p) && !sig_shutdown())
        sleep(1);

    /*
     * 清理：按创建顺序的逆序释放所有资源
     *
     * 顺序：
     *   1. sig_request_shutdown()：通知所有线程退出
     *   2. pipeline_stop(p)：等待线程结束 + 释放所有模块
     *   3. monitor_stop()：停止监控线程
     *   4. logger_shutdown()：关闭日志系统
     */
    LOG_INFO("Shutting down...");
    sig_request_shutdown();
    pipeline_stop(p);
    monitor_stop();
    logger_shutdown();

    return 0;
}
