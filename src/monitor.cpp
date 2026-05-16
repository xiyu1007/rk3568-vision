#include "monitor.hpp"
#include "logger.hpp"

#include <fstream>
#include <sstream>
#include <cstring>
#include <thread>
#include <chrono>

namespace rk3568_vision {

void SystemMonitor::start() {
    running_ = true;
    thread_ = std::thread(&SystemMonitor::run, this);
}

void SystemMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void SystemMonitor::run() {
    using namespace std::chrono_literals;

    while (running_) {
        // 读取 CPU 使用率（/proc/stat）
        {
            std::ifstream stat("/proc/stat");
            if (stat.is_open()) {
                std::string line;
                std::getline(stat, line);
                // 解析 cpu 行: cpu  user nice system idle ...
                static float prev_idle = 0, prev_total = 0;
                std::istringstream iss(line);
                std::string cpu;
                float user, nice, system, idle;
                iss >> cpu >> user >> nice >> system >> idle;
                float total = user + nice + system + idle;
                if (prev_total > 0 && total > prev_total) {
                    float usage = 100.0f * (1.0f - (idle - prev_idle) / (total - prev_total));
                    cpu_usage_ = std::max(0.0f, std::min(100.0f, usage));
                }
                prev_idle = idle;
                prev_total = total;
            }
        }

        // 读取内存使用率（/proc/meminfo）
        {
            std::ifstream meminfo("/proc/meminfo");
            if (meminfo.is_open()) {
                std::string label, value, unit;
                float total = 0, available = 0;
                for (int i = 0; i < 10; ++i) {
                    meminfo >> label >> value >> unit;
                    float val = std::stof(value);
                    if (label == "MemTotal:") total = val;
                    if (label == "MemAvailable:") available = val;
                }
                if (total > 0) mem_usage_ = 100.0f * (1.0f - available / total);
            }
        }

        // 读取 NPU 温度（Rockchip thermal zone，实际路径取决于 BSP）
        {
            std::ifstream temp("/sys/class/thermal/thermal_zone1/temp");
            if (temp.is_open()) {
                int raw_temp = 0;
                temp >> raw_temp;
                npu_temp_ = raw_temp / 1000.0f;
            }
        }

        // 每2秒采样一次
        for (int i = 0; i < 20 && running_; ++i) {
            std::this_thread::sleep_for(100ms);
        }
    }
}

} // namespace rk3568_vision
