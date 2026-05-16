#pragma once

#include "config.hpp"
#include "logger.hpp"
#include <opencv2/core.hpp>

namespace rk3568_vision {

class OSD {
public:
    void draw_detections(cv::Mat& img, const DetectResult& result);
    void draw_fps(cv::Mat& img, double fps);
    void draw_timestamp(cv::Mat& img, const std::string& ts);
};

} // namespace rk3568_vision
