// ============================================================================
// inferencer.cpp — RKNN 推理实现（前处理 + 推理 + YOLOv5 后处理 + 画框）
// ============================================================================

#include "vision/inferencer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

// RGA im2d 头文件自身通过 IM_API/IM_C_API 宏处理 C/C++ 兼容，勿再用 extern "C"
// 包裹（否则会破坏内部 C++ 重载声明导致冲突）。
#include <rga/im2d.h>

// ARM NEON SIMD（仅 aarch64，用于反量化加速）。
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "vision/debug.hpp"
#include "vision/logger.hpp"

namespace vision {

// ===========================================================================
//  常量与工具函数
// ===========================================================================
namespace {

// COCO 类别数。
constexpr int kNumClasses = 80;

// 每个 anchor 的预测值数量（4 边框 + 1 目标置信度 + 80 类别）。
constexpr int kBoxChannels = 85;

// 每个输出头的 anchor 数量。
constexpr int kNumAnchors = 3;

// YOLOv5s 预设 anchor（640×640 输入，单位像素）。
constexpr int kAnchors[3][6] = {
    {10, 13,   16, 30,   33, 23},      // stride 8   小目标
    {30, 61,   62, 45,   59, 119},     // stride 16  中目标
    {116, 90,  156, 198, 373, 326},    // stride 32  大目标
};

// sigmoid 激活函数（标准 yolov5s 后处理用）。
inline float Sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-value));
}

// 将 float 钳制到 [0, 255] 并转 uint8_t。
inline uint8_t ClampToUint8(float value) {
    if (value < 0.0f) {
        return 0;
    }
    if (value > 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

// 将 float 钳制到 [lo, hi] 并转 int。
inline int ClampToInt(float value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return static_cast<int>(value);
}

// int8 反量化：int8 → float。
inline float Dequantize(int8_t value, int32_t zero_point, float scale) {
    return (static_cast<float>(value) - static_cast<float>(zero_point)) * scale;
}

// 两个框的 IoU（左上角 + 宽高表示）。
inline float BoxIoU(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh) {
    const float x1 = std::max(ax, bx);
    const float y1 = std::max(ay, by);
    const float x2 = std::min(ax + aw, bx + bw);
    const float y2 = std::min(ay + ah, by + bh);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float union_area = aw * ah + bw * bh - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

// NMS 前的候选检测框。
struct Candidate {
    float x;          // 框左上角 X
    float y;          // 框左上角 Y
    float width;      // 框宽
    float height;     // 框高
    float score;      // 置信度
    int class_id;     // 类别 ID
};

// 一个反量化后的输出头。
struct OutputTensor {
    const float* data;   // 反量化后的数据指针
    int height;          // 网格高度
    int width;           // 网格宽度
    int stride;          // 下采样步长
};

} // namespace

// ===========================================================================
//  构造 / 析构
// ===========================================================================
Inferencer::Inferencer(const InferenceConfig& config) : config_(config) {}

Inferencer::~Inferencer() {
    if (context_ != 0) {
        rknn_destroy(context_);
        context_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Initialize：加载模型 + 标签，查询输入输出
// ---------------------------------------------------------------------------
bool Inferencer::Initialize() {
    // 1. 初始化 RKNN 上下文（size=0 表示 model 参数为文件路径）。
    const std::string path = config_.model_path;
    int ret = rknn_init(&context_, const_cast<char*>(path.c_str()), 0, 0, nullptr);
    if (ret < 0) {
        Logger::instance().error("rknn: init failed ret=%d", ret);
        return false;
    }

    // 2. 查询输入/输出数量。
    rknn_input_output_num io_num{};
    ret = rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        Logger::instance().error("rknn: query IO failed");
        return false;
    }
    output_count_ = io_num.n_output;

    // 3. 查询输入尺寸。
    rknn_tensor_attr input_attribute{};
    input_attribute.index = 0;
    rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attribute, sizeof(input_attribute));
    if (input_attribute.fmt == RKNN_TENSOR_NCHW) {
        model_height_ = input_attribute.dims[2];
        model_width_  = input_attribute.dims[3];
    } else {
        model_height_ = input_attribute.dims[1];
        model_width_  = input_attribute.dims[2];
    }
    Logger::instance().info("rknn: model input %ux%u, %u outputs",
                            model_width_, model_height_, output_count_);

    // 4. 查询输出属性并分配缓冲。
    if (!QueryOutputs()) {
        return false;
    }

    // 5. 加载标签。
    std::ifstream label_file(config_.labels_path);
    if (label_file.is_open()) {
        std::string line;
        while (std::getline(label_file, line)) {
            if (!line.empty()) {
                labels_.push_back(line);
            }
        }
    }
    Logger::instance().info("rknn: %zu labels loaded", labels_.size());

    rgb_buffer_.resize(static_cast<size_t>(model_width_) * model_height_ * 3);
    initialized_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// QueryOutputs：查询输出张量属性，初始化反量化缓冲
// ---------------------------------------------------------------------------
bool Inferencer::QueryOutputs() {
    output_attributes_.resize(output_count_);
    output_offsets_.resize(output_count_);
    size_t total_elements = 0;
    for (uint32_t i = 0; i < output_count_; ++i) {
        output_attributes_[i].index = i;
        const int ret = rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR,
                                   &output_attributes_[i], sizeof(rknn_tensor_attr));
        if (ret < 0) {
            Logger::instance().error("rknn: output attr[%u] failed", i);
            return false;
        }
        int elements = 1;
        for (uint32_t d = 0; d < output_attributes_[i].n_dims; ++d) {
            elements *= output_attributes_[i].dims[d];
        }
        output_offsets_[i] = total_elements;
        total_elements += elements;
        Logger::instance().info("rknn: output[%u] dims=[%u,%u,%u,%u] scale=%.4f zp=%d",
                                i,
                                output_attributes_[i].dims[0],
                                output_attributes_[i].dims[1],
                                output_attributes_[i].dims[2],
                                output_attributes_[i].dims[3],
                                output_attributes_[i].scale,
                                static_cast<int>(output_attributes_[i].zp));
    }
    dequant_buffer_.resize(total_elements);
    return true;
}

// ===========================================================================
//  Preprocess：RGA 硬件 NV12 → RGB letterbox（零拷贝）
// ===========================================================================
void Inferencer::Preprocess(const FramePtr& frame, uint8_t* rgb, LetterboxInfo& letterbox) {
    const int width = static_cast<int>(frame->width);
    const int height = static_cast<int>(frame->height);

    // 计算 letterbox 缩放参数。
    const float scale = std::min(static_cast<float>(model_width_) / width,
                                 static_cast<float>(model_height_) / height);
    const int scaled_width  = static_cast<int>(width * scale);
    const int scaled_height = static_cast<int>(height * scale);
    letterbox.scale = scale;
    letterbox.pad_x = (static_cast<int>(model_width_) - scaled_width) / 2;
    letterbox.pad_y = (static_cast<int>(model_height_) - scaled_height) / 2;

    // 灰边填充（YOLOv5 惯例 114）。
    std::memset(rgb, 114, static_cast<size_t>(model_width_) * model_height_ * 3);

    // 分配 RGA 中间缓冲（采集分辨率 RGB）。
    const size_t tmp_size = static_cast<size_t>(scaled_width) * scaled_height * 3;
    if (rgb_tmp_buffer_.size() < tmp_size) {
        rgb_tmp_buffer_.resize(tmp_size);
    }

    // RGA：NV12 → RGB 格式转换 + 等比缩放。
    // 注意：用 mmap 虚拟地址（Frame::nv12_data）而非 fd，以支持 V4L2 的 stride
    // （bytesperline 可能 != width）。数据在 DMA buffer 中，mmap 映射零拷贝。
    // 注意 wrapbuffer_virtualaddr 参数顺序：va, width, height, format, wstride, hstride。
    rga_buffer_t src = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(frame->nv12_data), width, height,
        RK_FORMAT_YCbCr_420_SP,
        static_cast<int>(frame->nv12_stride), height);
    rga_buffer_t dst = wrapbuffer_virtualaddr(
        rgb_tmp_buffer_.data(), scaled_width, scaled_height,
        RK_FORMAT_RGB_888,
        scaled_width, scaled_height);

    // fx=fy=0 表示按 dst/src 尺寸自动缩放；interpolation=0 用默认插值；sync=1 同步执行。
    const IM_STATUS status = imresize(src, dst);
    if (status == IM_STATUS_SUCCESS) {
        // RGA 成功：把缩放后的 RGB 拷到 letterbox 位置。
        for (int row = 0; row < scaled_height; ++row) {
            std::memcpy(rgb + (static_cast<size_t>(row + letterbox.pad_y) * model_width_ +
                               letterbox.pad_x) * 3,
                        rgb_tmp_buffer_.data() + static_cast<size_t>(row) * scaled_width * 3,
                        static_cast<size_t>(scaled_width) * 3);
        }
        return;
    }

    // RGA 失败兜底：CPU 浮点转换（保证功能不坏）。
    Logger::instance().warn("inferencer: RGA preprocess failed (%d), fallback to CPU",
                            static_cast<int>(status));
    const uint8_t* nv12 = frame->nv12_data;
    const uint8_t* y_plane  = nv12;
    const uint8_t* uv_plane = nv12 + static_cast<size_t>(frame->nv12_stride) * height;
    for (int dy = 0; dy < scaled_height; ++dy) {
        const int sy = std::min(static_cast<int>(dy / scale), height - 1);
        const uint8_t* y_row  = y_plane + static_cast<size_t>(sy) * frame->nv12_stride;
        const uint8_t* uv_row = uv_plane + static_cast<size_t>(sy / 2) * frame->nv12_stride;
        uint8_t* dst = rgb + (static_cast<size_t>(dy + letterbox.pad_y) * model_width_ +
                              letterbox.pad_x) * 3;
        for (int dx = 0; dx < scaled_width; ++dx) {
            const int sx = std::min(static_cast<int>(dx / scale), width - 1);
            const int yv = y_row[sx];
            const size_t uv_offset = (sx & ~1);
            const int u = uv_row[uv_offset];
            const int v = uv_row[uv_offset + 1];
            const float yf = (yv - 16) * 1.164f;
            const float uf = u - 128.0f;
            const float vf = v - 128.0f;
            dst[dx * 3 + 0] = ClampToUint8(yf + 1.596f * vf);
            dst[dx * 3 + 1] = ClampToUint8(yf - 0.391f * uf - 0.813f * vf);
            dst[dx * 3 + 2] = ClampToUint8(yf + 2.018f * uf);
        }
    }
}

// ===========================================================================
//  Detect：完整推理
// ===========================================================================
bool Inferencer::Detect(const FramePtr& frame) {
    if (!initialized_ || !frame) {
        return false;
    }
    VISION_PROFILE_SCOPE("inference");
    const TimestampUs detect_start_us = GetCurrentTimestampUs();

    // 1. 前处理。
    LetterboxInfo letterbox;
    Preprocess(frame, rgb_buffer_.data(), letterbox);
    const TimestampUs preprocess_end_us = GetCurrentTimestampUs();

    // 2. 设置输入 + 推理。
    rknn_input input{};
    input.index = 0;
    input.type  = RKNN_TENSOR_UINT8;
    input.fmt   = RKNN_TENSOR_NHWC;
    input.buf   = rgb_buffer_.data();
    input.size  = static_cast<uint32_t>(rgb_buffer_.size());
    if (rknn_inputs_set(context_, 1, &input) < 0) {
        Logger::instance().error("rknn: inputs_set failed");
        return false;
    }
    if (rknn_run(context_, nullptr) < 0) {
        Logger::instance().error("rknn: run failed");
        return false;
    }

    // 3. 获取输出（INT8）。
    std::vector<rknn_output> outputs(output_count_);
    for (uint32_t i = 0; i < output_count_; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 0;
        outputs[i].is_prealloc = 0;
    }
    if (rknn_outputs_get(context_, output_count_, outputs.data(), nullptr) < 0) {
        Logger::instance().error("rknn: outputs_get failed");
        return false;
    }
    const TimestampUs rknn_end_us = GetCurrentTimestampUs();

    // 4. 反量化 → 构造输出头。
    std::vector<OutputTensor> tensors;
    tensors.reserve(output_count_);
    for (uint32_t i = 0; i < output_count_; ++i) {
        const rknn_tensor_attr& attribute = output_attributes_[i];
        const int8_t* src = static_cast<int8_t*>(outputs[i].buf);
        float* dst = dequant_buffer_.data() + output_offsets_[i];
        int elements = 1;
        for (uint32_t d = 0; d < attribute.n_dims; ++d) {
            elements *= attribute.dims[d];
        }
        const float scale = attribute.scale;
        const float zero_point = static_cast<float>(attribute.zp);
#if defined(__aarch64__) || defined(__ARM_NEON)
        // NEON SIMD 加速：16 个 int8 一次反量化（int8→int16→int32→float，再减 zp 乘 scale）。
        const float32x4_t zero_point_v = vdupq_n_f32(zero_point);
        const float32x4_t scale_v = vdupq_n_f32(scale);
        int e = 0;
        for (; e + 16 <= elements; e += 16) {
            const int8x16_t v8 = vld1q_s8(src + e);
            const int16x8_t v16_low = vmovl_s8(vget_low_s8(v8));
            const int16x8_t v16_high = vmovl_s8(vget_high_s8(v8));
            const int32x4_t v32_0 = vmovl_s16(vget_low_s16(v16_low));
            const int32x4_t v32_1 = vmovl_s16(vget_high_s16(v16_low));
            const int32x4_t v32_2 = vmovl_s16(vget_low_s16(v16_high));
            const int32x4_t v32_3 = vmovl_s16(vget_high_s16(v16_high));
            float32x4_t f0 = vcvtq_f32_s32(v32_0);
            float32x4_t f1 = vcvtq_f32_s32(v32_1);
            float32x4_t f2 = vcvtq_f32_s32(v32_2);
            float32x4_t f3 = vcvtq_f32_s32(v32_3);
            f0 = vmulq_f32(vsubq_f32(f0, zero_point_v), scale_v);
            f1 = vmulq_f32(vsubq_f32(f1, zero_point_v), scale_v);
            f2 = vmulq_f32(vsubq_f32(f2, zero_point_v), scale_v);
            f3 = vmulq_f32(vsubq_f32(f3, zero_point_v), scale_v);
            vst1q_f32(dst + e, f0);
            vst1q_f32(dst + e + 4, f1);
            vst1q_f32(dst + e + 8, f2);
            vst1q_f32(dst + e + 12, f3);
        }
        for (; e < elements; ++e) {
            dst[e] = Dequantize(src[e], attribute.zp, attribute.scale);
        }
#else
        for (int e = 0; e < elements; ++e) {
            dst[e] = Dequantize(src[e], attribute.zp, attribute.scale);
        }
#endif
        const int grid_width  = attribute.dims[3];
        const int grid_height = attribute.dims[2];
        tensors.push_back({dst, grid_height, grid_width,
                           static_cast<int>(model_width_ / grid_width)});
    }

    // 5. YOLOv5 解码 + NMS（输出原图坐标系检测框）。
    const int image_width  = static_cast<int>(frame->width);
    const int image_height = static_cast<int>(frame->height);
    DetectResult result{};
    std::vector<Candidate> candidates;

    for (size_t o = 0; o < tensors.size() && o < 3; ++o) {
        const OutputTensor& tensor = tensors[o];
        const int grid_len = tensor.height * tensor.width;
        const int anchor_row = (tensor.stride == 8) ? 0 : (tensor.stride == 16) ? 1 : 2;
        for (int a = 0; a < kNumAnchors; ++a) {
            const int base = kBoxChannels * a;   // 该 anchor 的通道基址
            for (int gy = 0; gy < tensor.height; ++gy) {
                for (int gx = 0; gx < tensor.width; ++gx) {
                    const int idx = gy * tensor.width + gx;
                    const int off = base * grid_len + idx;   // NCHW 数据基偏移

                    // 标准 yolov5s 输出是 logits，需 sigmoid；relu 版输出已是 sigmoid 后值。
                    // 按配置 use_sigmoid 决定是否再 sigmoid（relu 版若再 sigmoid，obj 会全部
                    // 接近 0.5~1、阈值失效导致候选暴增、后处理极慢）。
                    float objectness = tensor.data[off + 4 * grid_len];
                    if (config_.use_sigmoid) {
                        objectness = Sigmoid(objectness);
                    }
                    if (objectness < config_.confidence_threshold) {
                        continue;
                    }

                    int   max_class = 0;
                    float max_class_score = tensor.data[off + 5 * grid_len];
                    for (int c = 1; c < kNumClasses; ++c) {
                        const float v = tensor.data[off + (5 + c) * grid_len];
                        if (v > max_class_score) {
                            max_class_score = v;
                            max_class = c;
                        }
                    }
                    if (config_.use_sigmoid) {
                        max_class_score = Sigmoid(max_class_score);
                    }
                    const float score = objectness * max_class_score;
                    if (score < config_.confidence_threshold) {
                        continue;
                    }

                    float tx = tensor.data[off + 0 * grid_len];
                    float ty = tensor.data[off + 1 * grid_len];
                    float tw = tensor.data[off + 2 * grid_len];
                    float th = tensor.data[off + 3 * grid_len];
                    if (config_.use_sigmoid) {
                        tx = Sigmoid(tx);
                        ty = Sigmoid(ty);
                        tw = Sigmoid(tw);
                        th = Sigmoid(th);
                    }
                    const float bx = (tx * 2.0f - 0.5f + gx) * tensor.stride;
                    const float by = (ty * 2.0f - 0.5f + gy) * tensor.stride;
                    const float bw = std::pow(tw * 2.0f, 2) * kAnchors[anchor_row][2 * a];
                    const float bh = std::pow(th * 2.0f, 2) * kAnchors[anchor_row][2 * a + 1];

                    // 模型坐标(640×640) → 原图坐标：减 padding 除 scale。
                    candidates.push_back({
                        (bx - bw * 0.5f - letterbox.pad_x) / letterbox.scale,
                        (by - bh * 0.5f - letterbox.pad_y) / letterbox.scale,
                        bw / letterbox.scale,
                        bh / letterbox.scale,
                        score,
                        max_class});
                }
            }
        }
    }

    // NMS：按类别抑制重叠框。
    std::vector<char> keep(candidates.size(), 1);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!keep[j] || candidates[i].class_id != candidates[j].class_id) {
                continue;
            }
            if (BoxIoU(candidates[i].x, candidates[i].y, candidates[i].width, candidates[i].height,
                       candidates[j].x, candidates[j].y, candidates[j].width, candidates[j].height)
                > config_.nms_threshold) {
                keep[j] = 0;
            }
        }
    }

    // 输出结果：钳制边界 + 填标签。
    for (size_t i = 0; i < candidates.size() && result.count < kMaxBoxes; ++i) {
        if (!keep[i]) {
            continue;
        }
        const Candidate& candidate = candidates[i];
        DetectBox& box = result.boxes[result.count];
        box.x = ClampToInt(candidate.x, 0, image_width);
        box.y = ClampToInt(candidate.y, 0, image_height);
        box.width  = ClampToInt(candidate.x + candidate.width, 0, image_width) - box.x;
        box.height = ClampToInt(candidate.y + candidate.height, 0, image_height) - box.y;
        box.class_id = candidate.class_id;
        box.confidence = candidate.score;
        if (candidate.class_id >= 0 &&
            static_cast<size_t>(candidate.class_id) < labels_.size()) {
            std::snprintf(box.label, sizeof(box.label), "%s",
                          labels_[candidate.class_id].c_str());
        } else {
            std::snprintf(box.label, sizeof(box.label), "cls_%d", candidate.class_id);
        }
        ++result.count;
    }

    frame->detection = result;
    frame->inference_timestamp = GetCurrentTimestampUs();

    VISION_DEBUG_LOG("detect: pre=%llu rknn=%llu post=%llu us",
                     static_cast<unsigned long long>(preprocess_end_us - detect_start_us),
                     static_cast<unsigned long long>(rknn_end_us - preprocess_end_us),
                     static_cast<unsigned long long>(frame->inference_timestamp - rknn_end_us));

    rknn_outputs_release(context_, output_count_, outputs.data());
    return true;
}

// ===========================================================================
//  DrawBoxesOnNv12：在 NV12 上画检测框（编码线程调用）
// ===========================================================================
void DrawBoxesOnNv12(uint8_t* nv12, int width, int height, int nv12_stride,
                     const DetectResult& detection) {
    uint8_t* y_plane = nv12;
    for (uint32_t i = 0; i < detection.count; ++i) {
        const DetectBox& box = detection.boxes[i];
        const int x0 = std::max(0, std::min(box.x, width - 1));
        const int y0 = std::max(0, std::min(box.y, height - 1));
        const int x1 = std::max(0, std::min(box.x + box.width - 1, width - 1));
        const int y1 = std::max(0, std::min(box.y + box.height - 1, height - 1));
        if (x1 < x0 || y1 < y0) {
            continue;
        }
        // 画上下两条水平边。
        for (int x = x0; x <= x1; ++x) {
            y_plane[static_cast<size_t>(y0) * nv12_stride + x] = 255;
            y_plane[static_cast<size_t>(y1) * nv12_stride + x] = 255;
        }
        // 画左右两条垂直边。
        for (int y = y0; y <= y1; ++y) {
            y_plane[static_cast<size_t>(y) * nv12_stride + x0] = 255;
            y_plane[static_cast<size_t>(y) * nv12_stride + x1] = 255;
        }
    }
}

} // namespace vision
