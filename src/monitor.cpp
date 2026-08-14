// ============================================================================
// monitor.cpp — 系统监控实现
// ============================================================================

#include "monitor.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "logger.hpp"
#include "perf.hpp"

namespace vision {

namespace {
// 温度传感器可能位于不同 thermal_zone，依次尝试。
const char* kThermalZones[] = {
    "/sys/class/thermal/thermal_zone0/temp",
    "/sys/class/thermal/thermal_zone1/temp",
};
} // namespace

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
Monitor& Monitor::instance() {
    static Monitor m;
    return m;
}

Monitor::~Monitor() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------
void Monitor::start(const MonitorConfig& cfg) {
    if (running_.exchange(true)) return;   // 已在运行
    cfg_ = cfg;
    thread_ = std::thread(&Monitor::run, this);
}

void Monitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------------------
// run：周期采样 + 输出状态日志
// ---------------------------------------------------------------------------
void Monitor::run() {
    while (running_.load()) {
        // 可中断的休眠（用多次短睡眠以便及时响应退出）。
        for (int waited = 0;
             waited < static_cast<int>(cfg_.log_interval_ms) && running_.load();
             waited += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!running_.load()) break;

        refresh();

        Perf& p = Perf::instance();
        LOG_INFO("monitor: cpu=%.1f%% mem=%.1f%% temp=%.1fC | "
                 "enc=%lldms inf=%lldms cap=%lldms | "
                 "frames=%lld drop=%lld push=%lld rec=%lld",
                 cpu_usage_.load(), mem_usage_.load(), temperature_.load(),
                 p.encode_us.load() / 1000, p.inference_us.load() / 1000,
                 p.capture_us.load() / 1000,
                 p.total_frames.load(), p.dropped_frames.load(),
                 p.pushed_frames.load(), p.recorded_frames.load());
    }
}

// ---------------------------------------------------------------------------
// refresh：采样 CPU/内存/温度
// ---------------------------------------------------------------------------
void Monitor::refresh() {
    uint64_t total = 0, idle = 0;
    if (readCpuStat(total, idle) && last_total_ != 0) {
        uint64_t dt = total - last_total_;
        uint64_t di = idle - last_idle_;
        if (dt > 0) {
            cpu_usage_.store(100.0 * (1.0 - static_cast<double>(di) / dt));
        }
    }
    last_total_ = total;
    last_idle_  = idle;

    double mem = 0.0;
    if (readMemUsage(mem)) mem_usage_.store(mem);

    double temp = 0.0;
    if (readTemperature(temp)) temperature_.store(temp);
}

// ---------------------------------------------------------------------------
// readCpuStat：读 /proc/stat 第一行（CPU 累计时间）
// ---------------------------------------------------------------------------
bool Monitor::readCpuStat(uint64_t& total, uint64_t& idle) {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return false;
    std::string line;
    std::getline(f, line);   // 第一行 "cpu  user nice system idle iowait ..."

    // 用 unsigned long long 与 %llu 严格匹配（避免 uint64_t 平台差异）。
    unsigned long long user = 0, nice = 0, sys = 0, id = 0, iowait = 0;
    if (std::sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu",
                    &user, &nice, &sys, &id, &iowait) < 4) {
        return false;
    }
    total = user + nice + sys + id + iowait;
    idle  = id + iowait;     // iowait 视为空闲
    return true;
}

// ---------------------------------------------------------------------------
// readMemUsage：读 /proc/meminfo
// ---------------------------------------------------------------------------
bool Monitor::readMemUsage(double& usage) {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return false;

    // 逐行解析 "key: value unit"（unit 可能缺失，故按行而非按 token 解析）。
    uint64_t total = 0, avail = 0;
    std::string line;
    while (std::getline(f, line)) {
        char key[64];
        unsigned long long val = 0;
        if (std::sscanf(line.c_str(), "%63s %llu", key, &val) < 2) continue;
        if (std::strcmp(key, "MemTotal:") == 0)     total = val;
        else if (std::strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    if (total == 0) return false;
    usage = 100.0 * (1.0 - static_cast<double>(avail) / total);
    return true;
}

// ---------------------------------------------------------------------------
// readTemperature：读 thermal_zone 温度（毫摄氏度 → 摄氏度）
// ---------------------------------------------------------------------------
bool Monitor::readTemperature(double& temp) {
    for (const char* path : kThermalZones) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        long millic = 0;
        if (f >> millic) {
            temp = static_cast<double>(millic) / 1000.0;
            return true;
        }
    }
    return false;
}

} // namespace vision
