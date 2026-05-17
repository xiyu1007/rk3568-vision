#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
// C types pulled in from types.h are available
#ifdef __cplusplus
}
#endif

#include "rknn_context.hpp"

#include <string>
#include <vector>
#include <memory>
#include <opencv2/core.hpp>

namespace rk3568_vision {

using DetectResult = detect_result_t;

class Detector {
public:
    Detector();
    ~Detector();

    bool init(const std::string& model_path, const std::string& labels_path,
              float conf = 0.25f, float nms = 0.45f, uint32_t npu_core = 0);
    DetectResult detect(const cv::Mat& bgr);

    uint32_t input_width()  const;
    uint32_t input_height() const;
    uint32_t output_count() const;

private:
    cv::Mat preprocess(const cv::Mat& bgr);

    std::unique_ptr<RknnContext> rknn_;
    std::vector<std::string>     labels_;
    float conf_threshold_ = 0.25f;
    float nms_threshold_  = 0.45f;
    bool  initialized_    = false;
};

}
