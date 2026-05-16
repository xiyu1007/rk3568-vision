#include "display.hpp"
#include "logger.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace rk3568_vision {

Display::Display(const std::string& window_name) : window_name_(window_name) {
    cv::namedWindow(window_name_, cv::WINDOW_NORMAL);
    cv::resizeWindow(window_name_, 960, 540);
    open_ = true;
    LOG_INFO("Display window created: %s", window_name_.c_str());
}

Display::~Display() { close(); }

void Display::show(const cv::Mat& img, bool show_fps, double fps) {
    if (!open_ || img.empty()) return;

    if (show_fps && fps > 0) {
        char text[32];
        snprintf(text, sizeof(text), "FPS: %.1f", fps);
        cv::putText(img, text, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX,
                    0.8, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }

    cv::imshow(window_name_, img);
    cv::waitKey(1);
}

void Display::close() {
    if (open_.exchange(false)) {
        cv::destroyWindow(window_name_);
    }
}

} // namespace rk3568_vision
