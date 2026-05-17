#pragma once

#include <opencv2/core.hpp>
#include <string>

namespace rk3568_vision {

class Display {
public:
    explicit Display(const std::string& name = "RK3568 Vision");
    ~Display();
    void show_frame(const cv::Mat& img);
    void close();
    bool is_open() const { return open_; }

private:
    std::string window_name_;
    bool open_ = false;
};

}
