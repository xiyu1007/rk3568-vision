// ============================================================================
// inferencer.hpp — RKNN 目标检测推理器
// ============================================================================
//
// 完整推理链路：NV12 → RGB letterbox → NPU 推理(YOLOv5s INT8) → 解码 + NMS
// → 原图坐标系检测框。所有前后处理（含 YOLOv5 后处理、NV12 画框）都在此模块。
// ============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "rknn_api.h"   // Rockchip RKNN C API

namespace vision {

// letterbox 缩放信息（前处理生成，后处理逆映射用）。
struct LetterboxInfo {
    float scale = 1.0f;   // 缩放比例 min(dst_w/w, dst_h/h)
    int   pad_x = 0;      // 水平灰边填充
    int   pad_y = 0;      // 垂直灰边填充
};

class Inferencer {
public:
    explicit Inferencer(const InferenceConfig& cfg);
    ~Inferencer();

    Inferencer(const Inferencer&) = delete;
    Inferencer& operator=(const Inferencer&) = delete;

    bool init();                        // 加载模型 + 标签
    bool detect(const FramePtr& frame); // 推理一帧，结果写回 frame->detect
    bool valid() const { return inited_; }

private:
    // 前处理：NV12 → RGB(letterbox)
    void preprocess(const FramePtr& f, uint8_t* rgb, LetterboxInfo& lb);
    // 查询输出张量属性，初始化反量化缓冲
    bool queryOutputs();

    InferenceConfig cfg_;
    rknn_context ctx_ = 0;
    uint32_t model_w_ = 640, model_h_ = 640;
    uint32_t n_outputs_ = 0;

    std::vector<rknn_tensor_attr> out_attrs_;   // 输出张量属性（scale/zp）
    std::vector<size_t>           out_offsets_; // 各输出在反量化缓冲中的偏移
    std::vector<uint8_t> rgb_buf_;              // 前处理 RGB 输入
    std::vector<float>   dequant_buf_;          // 反量化后的 float 输出
    std::vector<std::string> labels_;           // 类别标签

    bool inited_ = false;
};

// 在 NV12 图像上绘制检测框（编码线程在编码前调用，就地修改）。
void drawBoxesNv12(uint8_t* nv12, int w, int h, const DetectResult& det);

} // namespace vision
