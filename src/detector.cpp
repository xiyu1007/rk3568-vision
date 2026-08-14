// ============================================================================
// detector.cpp — YOLOv5 后处理实现
// ============================================================================

#include "detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "logger.hpp"

namespace vision {

namespace {

constexpr int   kNumClasses = 80;   // COCO 类别数
constexpr int   kBoxChannels = 85;  // 每个 anchor 的预测值数（4+1+80）
constexpr int   kNumAnchors = 3;    // 每个输出头的 anchor 数

// YOLOv5s 预设 anchor（在 640×640 输入尺度下，单位像素）。
// 三个输出头（stride 8/16/32）各 3 个 anchor（宽、高交替）。
constexpr int kAnchors[3][6] = {
    {10, 13,   16, 30,   33, 23},      // stride 8  (小目标)
    {30, 61,   62, 45,   59, 119},     // stride 16 (中目标)
    {116, 90,  156, 198, 373, 326},    // stride 32 (大目标)
};

// sigmoid：把任意实数映射到 (0,1)，用于回归参数与置信度。
inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// 钳制浮点值到 [min,max]。
inline int clampInt(float v, int min, int max) {
    if (v < min) return min;
    if (v > max) return max;
    return static_cast<int>(v);
}

// 计算两个框的 IoU（交并比）。框用左上角 + 宽高表示。
inline float iou(float ax, float ay, float aw, float ah,
                 float bx, float by, float bw, float bh) {
    float x1 = std::max(ax, bx);
    float y1 = std::max(ay, by);
    float x2 = std::min(ax + aw, bx + bw);
    float y2 = std::min(ay + ah, by + bh);
    float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float uni = aw * ah + bw * bh - inter;
    return (uni > 0.0f) ? (inter / uni) : 0.0f;
}

// 候选检测（NMS 前的中间结构）。
struct Candidate {
    float x, y, w, h;   // 左上角 + 宽高（原图坐标系）
    float score;
    int   cls;
};

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
YoloDecoder::YoloDecoder(float conf_threshold, float nms_threshold,
                         std::vector<std::string> labels)
    : conf_threshold_(conf_threshold),
      nms_threshold_(nms_threshold),
      labels_(std::move(labels)) {}

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------
DetectResult YoloDecoder::decode(const std::vector<OutputTensor>& outputs,
                                 const LetterboxInfo& lb, int img_w, int img_h) {
    DetectResult result{};
    std::vector<Candidate> cands;

    // ---- 遍历每个输出头（stride 8/16/32）----
    for (size_t o = 0; o < outputs.size() && o < 3; ++o) {
        const OutputTensor& t = outputs[o];
        const int glen = t.h * t.w;   // 该尺度网格点总数

        // 依据 stride 选择对应尺度的 anchor 组（与输出张量的排列顺序无关，
        // 即使模型输出的三个头顺序不同也能正确匹配）。
        const int anchor_row = (t.stride == 8) ? 0 : (t.stride == 16) ? 1 : 2;

        // ---- 遍历 3 个 anchor ----
        for (int a = 0; a < kNumAnchors; ++a) {
            const int base = kBoxChannels * a;   // 该 anchor 的通道基址

            // ---- 遍历每个网格点 ----
            for (int gy = 0; gy < t.h; ++gy) {
                for (int gx = 0; gx < t.w; ++gx) {
                    const int idx = gy * t.w + gx;          // 空间索引
                    const int off = base * glen + idx;      // 数据基偏移（NCHW）

                    // objectness（通道 4）→ sigmoid。
                    float obj = sigmoid(t.data[off + 4 * glen]);
                    if (obj < conf_threshold_) continue;    // 快速剪枝

                    // 找 80 类中概率最大的类别（通道 5~84）。
                    int   max_cls = 0;
                    float max_val = t.data[off + 5 * glen];
                    for (int c = 1; c < kNumClasses; ++c) {
                        float v = t.data[off + (5 + c) * glen];
                        if (v > max_val) { max_val = v; max_cls = c; }
                    }
                    float cls_prob = sigmoid(max_val);
                    float score = obj * cls_prob;           // 综合置信度
                    if (score < conf_threshold_) continue;

                    // 边界框解码（YOLOv5 公式）。
                    float tx = sigmoid(t.data[off + 0 * glen]);
                    float ty = sigmoid(t.data[off + 1 * glen]);
                    float tw = sigmoid(t.data[off + 2 * glen]);
                    float th = sigmoid(t.data[off + 3 * glen]);
                    float bx = (tx * 2.0f - 0.5f + gx) * t.stride;
                    float by = (ty * 2.0f - 0.5f + gy) * t.stride;
                    float bw = std::pow(tw * 2.0f, 2) * kAnchors[anchor_row][2 * a];
                    float bh = std::pow(th * 2.0f, 2) * kAnchors[anchor_row][2 * a + 1];

                    // 模型坐标(640×640) → 原图坐标：先减 padding 再除 scale。
                    float ox = (bx - bw * 0.5f - lb.pad_x) / lb.scale;
                    float oy = (by - bh * 0.5f - lb.pad_y) / lb.scale;
                    float ow = bw / lb.scale;
                    float oh = bh / lb.scale;

                    cands.push_back({ox, oy, ow, oh, score, max_cls});
                }
            }
        }
    }

    // ---- NMS：按类别抑制重叠框（O(n²)，候选框通常 < 100，足够快）----
    std::vector<char> keep(cands.size(), 1);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (!keep[j] || cands[i].cls != cands[j].cls) continue;
            if (iou(cands[i].x, cands[i].y, cands[i].w, cands[i].h,
                    cands[j].x, cands[j].y, cands[j].w, cands[j].h) > nms_threshold_) {
                keep[j] = 0;
            }
        }
    }

    // ---- 输出结果：钳制边界 + 填充标签 ----
    for (size_t i = 0; i < cands.size() && result.count < kMaxBoxes; ++i) {
        if (!keep[i]) continue;
        const Candidate& c = cands[i];
        DetectBox& b = result.boxes[result.count];

        b.x = clampInt(c.x, 0, img_w);
        b.y = clampInt(c.y, 0, img_h);
        b.w = clampInt(c.x + c.w, 0, img_w) - b.x;
        b.h = clampInt(c.y + c.h, 0, img_h) - b.y;
        b.class_id = c.cls;
        b.conf = c.score;
        if (c.cls >= 0 && static_cast<size_t>(c.cls) < labels_.size()) {
            std::snprintf(b.label, sizeof(b.label), "%s", labels_[c.cls].c_str());
        } else {
            std::snprintf(b.label, sizeof(b.label), "cls_%d", c.cls);
        }
        ++result.count;
    }

    return result;
}

} // namespace vision
