#pragma once

#include "types.hpp"
#include "rknn_context.hpp"

#include <string>
#include <vector>
#include <memory>
#include <opencv2/core.hpp>

namespace rk3568_vision {

// ============================================================================
// Detector — YOLOv5 目标检测器封装
//
// 处理流程:
//   1. 预处理: BGR -> RGB, RGA resize (或 OpenCV letterbox)
//   2. 推理:   RKNN run (NPU 硬件加速)
//   3. 后处理: 量化反量化 -> NMS -> 框体坐标映射回原始分辨率
//
// 性能关键路径:
//   - 预处理使用 RGA 硬件缩放（RK3568 内置），比 OpenCV 快 10x
//   - 后处理在 CPU 执行，使用 SIMD 优化（-O3 -ftree-vectorize）
//   - 输入 tensor 格式为 NHWC INT8，无需额外转换
// ============================================================================
class Detector {
public:
    Detector();
    ~Detector();

    bool init(const std::string& model_path, const std::string& labels_path,
              float conf_threshold = 0.25f, float nms_threshold = 0.45f,
              uint32_t npu_core = 0);

    DetectResult detect(const cv::Mat& bgr_image);

    bool is_initialized() const { return initialized_; }
    uint32_t input_width()  const;
    uint32_t input_height() const;
    uint32_t output_count() const;

private:
    cv::Mat preprocess(const cv::Mat& bgr);
    DetectResult postprocess(int8_t** outputs, int img_w, int img_h,
                             float scale_w, float scale_h);

    std::unique_ptr<RknnContext> rknn_;
    std::vector<std::string>     labels_;
    float conf_threshold_ = 0.25f;
    float nms_threshold_  = 0.45f;
    bool  initialized_    = false;
};

} // namespace rk3568_vision
