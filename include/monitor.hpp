// ============================================================================
// monitor.hpp — 系统监控（单例）
// ============================================================================
//
// 职责：周期读取系统资源状态（CPU/内存/温度），并把各阶段性能统计与
//       队列深度汇总成一条状态日志输出，便于观察系统是否健康。
//
// 设计要点：
//   - 【单例】全局唯一，流水线与监控线程通过 Monitor::instance() 访问
//   - 【独立线程】周期唤醒（默认 5s），不干扰实时流水线
//   - 【原子暴露】cpu/mem/temp 以 std::atomic<double> 暴露，读取无需加锁
// ============================================================================

#pragma once

#include <atomic>
#include <thread>

#include "config.hpp"

namespace vision {

class Monitor {
public:
    static Monitor& instance();

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    // 启动监控线程。
    void start(const MonitorConfig& cfg);
    // 停止监控线程。
    void stop();

    // 最近一次采样值。
    double cpuUsage()    const { return cpu_usage_.load(); }
    double memUsage()    const { return mem_usage_.load(); }
    double temperature() const { return temperature_.load(); }

private:
    Monitor() = default;
    ~Monitor();

    void run();            // 线程主循环
    void refresh();        // 采样一次 CPU/内存/温度

    // 读取 /proc/stat 的 CPU 累计时间（返回 total 与 idle）。
    bool readCpuStat(uint64_t& total, uint64_t& idle);
    // 读取 /proc/meminfo 的内存使用率（0~100）。
    bool readMemUsage(double& usage);
    // 读取 /sys/class/thermal 下的温度（摄氏度）。
    bool readTemperature(double& temp);

    MonitorConfig cfg_;
    std::thread    thread_;
    std::atomic<bool> running_{false};

    // CPU 快照（上次采样，用于差值计算）。
    uint64_t last_total_ = 0;
    uint64_t last_idle_  = 0;

    std::atomic<double> cpu_usage_{0.0};
    std::atomic<double> mem_usage_{0.0};
    std::atomic<double> temperature_{0.0};
};

} // namespace vision
