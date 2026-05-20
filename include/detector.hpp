/**
 * ==========================================================================
 * detector.hpp — YOLOv5 目标检测器头文件
 * ==========================================================================
 *
 * **Detector 类**：封装完整的 YOLOv5 推理管线
 *   1. 持有 RknnContext（NPU 模型管理）
 *   2. 前处理：BGR→RGB + resize
 *   3. 调用 NPU 推理
 *   4. 后处理：INT8 反量化 + 解码框 + NMS
 *
 * **与 C 代码的集成**：
 *   bridge.cpp 中的 bridge_detector_* 函数将 C++ Detector 包装为 void* 接口
 *   pipeline.c 通过 bridge 函数调用 Detector 的功能
 */

#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
/* C types pulled in from types.h are available */
#ifdef __cplusplus
}
#endif

#include "rknn_context.hpp"

#include <string>
#include <vector>
#include <memory>
#include <opencv2/core.hpp>

namespace rk3568_vision {

using DetectResult = detect_result_t;  /* C 兼容的检测结果类型 */

class Detector {
public:
    Detector();
    ~Detector();

    /*
     * 初始化检测器
     * @model_path：.rknn 模型文件路径
     * @labels_path：类别标签文件路径（COCO 80 类）
     * @conf：置信度阈值（0.25 表示 25% 置信度以下丢弃）
     * @nms：NMS IoU 阈值（0.45 表示重叠度 45% 以上抑制）
     * @npu_core：NPU 核心号
     */
    bool init(const std::string& model_path, const std::string& labels_path,
              float conf = 0.25f, float nms = 0.45f, uint32_t npu_core = 0);

    /*
     * 执行目标检测
     * 输入 BGR 图像，返回检测结果（检测框列表）
     * 这是完整的前处理 + 推理 + 后处理管道
     */
    DetectResult detect(const cv::Mat& bgr);

    /* 属性查询 */
    uint32_t input_width()  const;   /* 模型输入宽度（通常 640） */
    uint32_t input_height() const;   /* 模型输入高度（通常 640） */
    uint32_t output_count() const;   /* 输出头数量（YOLOv5=3）  */

private:
    /* 图像预处理：BGR→RGB + resize 到模型输入尺寸 */
    cv::Mat preprocess(const cv::Mat& bgr);

    /* 成员变量 */
    std::unique_ptr<RknnContext> rknn_;     /* NPU 上下文（RAII 管理）   */
    std::vector<std::string>     labels_;   /* COCO 80 类标签列表        */
    float conf_threshold_ = 0.25f;           /* 置信度阈值               */
    float nms_threshold_  = 0.45f;           /* NMS 阈值                 */
    bool  initialized_    = false;           /* 初始化完成标志           */
};

} // namespace rk3568_vision
