#include "osd.hpp"
#include <opencv2/imgproc.hpp>

namespace rk3568_vision {

void OSD::draw_detections(cv::Mat& img, const DetectResult& result) {
    for (uint32_t i = 0; i < result.count; ++i) {
        auto& b = result.boxes[i];
        cv::rectangle(img, cv::Rect(b.x, b.y, b.width, b.height),
                      cv::Scalar(0, 255, 0), 2);
        char label[128];
        snprintf(label, sizeof(label), "%s %.1f%%", b.label, b.confidence * 100.0f);
        cv::putText(img, label, cv::Point(b.x, b.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 1);
    }
}

void OSD::draw_fps(cv::Mat& img, double fps) {
    char buf[32];
    snprintf(buf, sizeof(buf), "FPS: %.1f", fps);
    cv::putText(img, buf, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
}

void OSD::draw_timestamp(cv::Mat& img, const std::string& ts) {
    cv::putText(img, ts, cv::Point(15, 65), cv::FONT_HERSHEY_SIMPLEX,
                0.5, cv::Scalar(255, 255, 255), 1);
}

} // namespace rk3568_vision
