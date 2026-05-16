#pragma once

#include "types.hpp"

#include <opencv2/core.hpp>
#include <string>
#include <atomic>

namespace rk3568_vision {

// ============================================================================
// Display — OpenCV 本地显示窗口
// ============================================================================
class Display {
public:
    explicit Display(const std::string& window_name = "RK3568 Vision");
    ~Display();

    void show(const cv::Mat& img, bool show_fps = true, double fps = 0.0);
    void close();

    bool is_open() const { return open_.load(std::memory_order_relaxed); }

private:
    std::string window_name_;
    std::atomic<bool> open_{false};
};

} // namespace rk3568_vision
