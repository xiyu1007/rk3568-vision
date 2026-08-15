// ============================================================================
// inferencer.hpp — RKNN 目标检测推理器
// ============================================================================
//
// 完整推理链路：NV12 → RGB letterbox（RGA 硬件加速）→ NPU 推理(YOLOv5 INT8)
// → 反量化 + 解码 + NMS → 原图坐标系检测框。
//
// 设计要点：
//   - RGA 前处理支持两种输入：dmabuf fd（V4L2 零拷贝帧，importbuffer_fd）
//     与虚拟地址（mp4 帧，wrapbuffer_virtualaddr），避免 CPU 拷贝。
//   - 后处理是否 sigmoid 由配置 use_sigmoid 决定（标准 yolov5s=true，relu 版=false）。
// ============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vision/config.hpp"
#include "vision/types.hpp"

#include "rknn_api.h"   // Rockchip RKNN C API

namespace vision {

// letterbox 缩放信息（前处理生成，后处理逆映射用）。
struct LetterboxInfo {
    float scale = 1.0f;   // 缩放比例 min(dst_w/w, dst_h/h)
    int pad_x = 0;        // 水平灰边填充
    int pad_y = 0;        // 垂直灰边填充
};

class Inferencer {
public:
    explicit Inferencer(const InferenceConfig& config);
    ~Inferencer();

    Inferencer(const Inferencer&) = delete;
    Inferencer& operator=(const Inferencer&) = delete;

    // 加载模型 + 标签，初始化 RGA/NPU 资源。
    bool Initialize();

    // 推理一帧，检测结果写回 frame->detection。
    bool Detect(const FramePtr& frame);

    bool IsValid() const { return initialized_; }

private:
    // RGA 前处理：NV12 → RGB letterbox。
    void Preprocess(const FramePtr& frame, uint8_t* rgb, LetterboxInfo& letterbox);

    // 查询输出张量属性，初始化反量化缓冲。
    bool QueryOutputs();

    InferenceConfig config_;
    rknn_context context_ = 0;
    uint32_t model_width_ = 640;     // 模型输入宽
    uint32_t model_height_ = 640;    // 模型输入高
    uint32_t output_count_ = 0;      // 输出张量数量

    std::vector<rknn_tensor_attr> output_attributes_;   // 输出张量属性（scale/zp）
    std::vector<size_t>           output_offsets_;      // 各输出在反量化缓冲中的偏移
    std::vector<uint8_t> rgb_buffer_;                   // 模型输入 RGB（640x640）
    std::vector<uint8_t> rgb_tmp_buffer_;               // RGA 中间缓冲（采集分辨率 RGB）
    std::vector<float>   dequant_buffer_;               // 反量化后的 float 输出
    std::vector<std::string> labels_;                   // 类别标签

    bool initialized_ = false;
};

// 在 NV12 图像上绘制检测框（编码线程在编码前调用，就地修改）。
// nv12_stride 为 NV12 行跨度（V4L2 的 bytesperline，mp4 时=width）。
void DrawBoxesOnNv12(uint8_t* nv12, int width, int height, int nv12_stride,
                     const DetectResult& detection);

} // namespace vision
