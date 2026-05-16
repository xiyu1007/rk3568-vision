#pragma once

#include <atomic>
#include <thread>

namespace rk3568_vision {

class SystemMonitor {
public:
    void start();
    void stop();

    float cpu_usage()  const { return cpu_usage_.load(std::memory_order_relaxed); }
    float mem_usage()  const { return mem_usage_; }
    float npu_temp()   const { return npu_temp_.load(std::memory_order_relaxed); }

private:
    void run();

    std::atomic<bool>   running_{false};
    std::thread         thread_;
    std::atomic<float>  cpu_usage_{0.0f};
    std::atomic<float>  npu_temp_{0.0f};
    float               mem_usage_ = 0.0f;
};

} // namespace rk3568_vision
