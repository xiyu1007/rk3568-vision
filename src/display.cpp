#include "display.hpp"

extern "C" {
#include "logger.h"
}

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace rk3568_vision {

Display::Display(const std::string& name) : window_name_(name) {
    cv::namedWindow(window_name_, cv::WINDOW_NORMAL);
    cv::resizeWindow(window_name_, 960, 540);
    open_ = true;
    LOG_INFO("display created: %s", name.c_str());
}

Display::~Display() { close(); }

void Display::show_frame(const cv::Mat& img) {
    if (!open_ || img.empty()) return;
    cv::imshow(window_name_, img);
    cv::waitKey(1);
}

void Display::close() {
    if (!open_) return;
    open_ = false;
    cv::destroyWindow(window_name_);
}

} // namespace rk3568_vision
