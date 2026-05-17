#include "bridge.h"
#include "logger.h"

extern "C" {
#include "bridge.h"
}

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

/* ── Detector bridge ──────────────────────────────────────────────────── */

#include "detector.hpp"

void* bridge_detector_create(void) {
    return new rk3568_vision::Detector();
}

void bridge_detector_destroy(void* d) {
    delete static_cast<rk3568_vision::Detector*>(d);
}

int bridge_detector_init(void* d, const char* model, const char* labels,
                          float conf, float nms, uint32_t npu_core) {
    auto* det = static_cast<rk3568_vision::Detector*>(d);
    return det->init(model, labels, conf, nms, npu_core) ? 1 : 0;
}

void bridge_detector_detect(void* d, const uint8_t* bgr, int w, int h,
                             detect_result_t* result) {
    auto* det = static_cast<rk3568_vision::Detector*>(d);
    cv::Mat img(h, w, CV_8UC3, const_cast<uint8_t*>(bgr));
    auto r = det->detect(img);
    result->count = r.count;
    for (uint32_t i = 0; i < r.count && i < DETECT_MAX_BOXES; i++) {
        result->boxes[i].x        = r.boxes[i].x;
        result->boxes[i].y        = r.boxes[i].y;
        result->boxes[i].w        = r.boxes[i].w;
        result->boxes[i].h        = r.boxes[i].h;
        result->boxes[i].class_id = r.boxes[i].class_id;
        result->boxes[i].conf     = r.boxes[i].conf;
        strncpy(result->boxes[i].label, r.boxes[i].label, FRAME_LABEL_MAX - 1);
    }
}

int bridge_detector_input_w(void* d) {
    return (int)static_cast<rk3568_vision::Detector*>(d)->input_width();
}

int bridge_detector_input_h(void* d) {
    return (int)static_cast<rk3568_vision::Detector*>(d)->input_height();
}

/* ── Display bridge ───────────────────────────────────────────────────── */

void* bridge_display_create(const char* name) {
    cv::namedWindow(name, cv::WINDOW_NORMAL);
    cv::resizeWindow(name, 960, 540);
    return new cv::String(name);
}

void bridge_display_destroy(void* d) {
    if (!d) return;
    cv::destroyWindow(static_cast<cv::String*>(d)->c_str());
    delete static_cast<cv::String*>(d);
}

void bridge_display_show(void* d, const uint8_t* bgr, int w, int h,
                          int show_fps, double fps,
                          const detect_result_t* detections) {
    if (!d) return;
    cv::Mat img(h, w, CV_8UC3, const_cast<uint8_t*>(bgr));
    cv::Mat disp = img.clone();

    if (show_fps && fps > 0) {
        char text[32];
        snprintf(text, sizeof(text), "FPS: %.1f", fps);
        cv::putText(disp, text, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX,
                    0.8, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }

    if (detections) {
        for (uint32_t i = 0; i < detections->count; i++) {
            auto& b = detections->boxes[i];
            cv::rectangle(disp, cv::Rect(b.x, b.y, b.w, b.h),
                         cv::Scalar(0, 255, 0), 2);
            char label[128];
            snprintf(label, sizeof(label), "%s %.1f%%",
                     b.label, b.conf * 100.0f);
            cv::putText(disp, label, cv::Point(b.x, b.y - 5),
                       cv::FONT_HERSHEY_SIMPLEX, 0.5,
                       cv::Scalar(0, 255, 0), 1);
        }
    }

    cv::imshow(static_cast<cv::String*>(d)->c_str(), disp);
    cv::waitKey(1);
}
