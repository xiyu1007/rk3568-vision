// ============================================================================
// rknn_inferencer.hpp — 真实 RKNN NPU 推理器（仅 aarch64）
// ============================================================================
//
// 职责：在 RK3568 上调用 RKNN C API，运行 YOLOv5s INT8 模型完成目标检测。
//
// 整个文件用 VISION_RK3568 宏保护：只有 aarch64 目标才编译，
// x86 开发机不会引入 rknn_api.h 与 librknnrt.so 的依赖。
// ============================================================================

#pragma once

#ifdef VISION_RK3568

#include <memory>
#include <string>
#include <vector>

#include "detector.hpp"
#include "inferencer.hpp"

#include "rknn_api.h"   // Rockchip 官方 RKNN SDK 头文件

namespace vision {

class RknnInferencer : public Inferencer {
public:
    explicit RknnInferencer(const Config& cfg);
    ~RknnInferencer() override;

    bool init() override;
    bool detect(const FramePtr& frame) override;
    const char* name() const override { return "rknn"; }

private:
    // 查询输出张量属性，初始化反量化缓冲与解码器所需的网格尺寸。
    bool queryOutputs();

    Config cfg_;                        // 配置副本（模型路径、阈值等）
    rknn_context ctx_ = 0;              // RKNN 运行时上下文句柄

    uint32_t model_w_ = 640;            // 模型输入宽（YOLOv5s 固定 640）
    uint32_t model_h_ = 640;            // 模型输入高
    uint32_t n_outputs_ = 0;            // 输出张量数量（应为 3）

    std::vector<rknn_tensor_attr> out_attrs_;   // 各输出张量属性（含 scale/zp）
    std::vector<size_t>            out_offsets_; // 各输出在反量化缓冲中的偏移

    std::vector<uint8_t> rgb_buf_;     // 前处理后的 RGB 输入（640×640×3）
    std::vector<float>   dequant_buf_; // 反量化后的 float 输出汇总缓冲

    std::vector<std::string>      labels_;   // 类别标签
    std::unique_ptr<YoloDecoder>  decoder_;  // 后处理解码器

    bool inited_ = false;
};

} // namespace vision

#endif // VISION_RK3568
