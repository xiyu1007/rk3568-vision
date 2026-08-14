// ============================================================================
// detector.hpp — YOLOv5 后处理（边界框解码 + NMS）
// ============================================================================
//
// 职责：把 NPU 输出的三个原始特征图（已反量化为 float32）解码成
//       原图像素坐标系下的检测框列表。
//
// YOLOv5s 模型输出结构：
//   - 三个输出头（stride 8/16/32），分别对应 80×80 / 40×40 / 20×20 网格
//   - 每个输出张量形状 [1, 255, H, W]（NCHW），255 = 3 anchor × 85
//   - 每个 anchor 的 85 个值 = [tx, ty, tw, th, objectness, 80 个类别概率]
//
// 关键处理步骤：
//   1. 每个网格点、每个 anchor 计算 objectness × class_prob 综合置信度
//   2. 边界框解码：bx=(σ(tx)*2-0.5+gx)*stride，bw=(σ(tw)*2)^2*anchor_w
//   3. 置信度过滤 → 按类别 NMS 去重
//   4. 模型坐标(640×640) 经 letterbox 逆映射回原图坐标
// ============================================================================

#pragma once

#include <string>
#include <vector>

#include "image_utils.hpp"
#include "types.hpp"

namespace vision {

// 一个反量化后的输出张量（float32）。
struct OutputTensor {
    const float* data = nullptr;   // float32 数据（NCHW 布局 [1, 255, H, W]）
    int          h = 0;            // 网格高度（如 80）
    int          w = 0;            // 网格宽度（如 80）
    int          stride = 8;       // 下采样倍率（8/16/32）
};

class YoloDecoder {
public:
    // 构造：传入阈值与类别标签表。
    YoloDecoder(float conf_threshold, float nms_threshold,
                std::vector<std::string> labels);

    // 解码三个输出头，输出原图坐标系下的检测结果。
    //   outputs : 三个输出张量（已反量化 float32）
    //   lb      : letterbox 信息（用于模型坐标 → 原图坐标逆映射）
    //   img_w/h : 原始图像宽高（框坐标的最终参考系）
    DetectResult decode(const std::vector<OutputTensor>& outputs,
                        const LetterboxInfo& lb, int img_w, int img_h);

private:
    float conf_threshold_;
    float nms_threshold_;
    std::vector<std::string> labels_;
};

} // namespace vision
