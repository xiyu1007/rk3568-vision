// ============================================================================
// inferencer.cpp — RKNN 推理实现（前处理 + 推理 + YOLOv5 后处理 + 画框）
// ============================================================================

#include "inferencer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "logger.hpp"

// RGA im2d 头文件自身通过 IM_API/IM_C_API 宏处理 C/C++ 兼容，勿再用 extern "C"
// 包裹（否则会破坏内部 C++ 重载声明导致冲突）。
#include <rga/im2d.h>

namespace vision {

// ===========================================================================
//  常量与工具函数
// ===========================================================================
namespace {

constexpr int kNumClasses = 80;   // COCO 类别数
constexpr int kBoxChannels = 85;  // 每 anchor 预测值数（4 + 1 + 80）
constexpr int kNumAnchors = 3;    // 每输出头 anchor 数

// YOLOv5s 预设 anchor（640×640 输入，单位像素）。
constexpr int kAnchors[3][6] = {
    {10, 13,   16, 30,   33, 23},      // stride 8  小目标
    {30, 61,   62, 45,   59, 119},     // stride 16 中目标
    {116, 90,  156, 198, 373, 326},    // stride 32 大目标
};

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

inline uint8_t clamp8(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return static_cast<uint8_t>(v);
}

inline int clampInt(float v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return static_cast<int>(v);
}

// 反量化：int8 → float。
inline float dequant(int8_t v, int32_t zp, float scale) {
    return (static_cast<float>(v) - static_cast<float>(zp)) * scale;
}

// 两个框的 IoU（左上角 + 宽高表示）。
inline float boxIoU(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh) {
    float x1 = std::max(ax, bx), y1 = std::max(ay, by);
    float x2 = std::min(ax + aw, bx + bw), y2 = std::min(ay + ah, by + bh);
    float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float uni = aw * ah + bw * bh - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// 候选检测（NMS 前）。
struct Candidate { float x, y, w, h, score; int cls; };

// 一个反量化后的输出头。
struct OutputTensor { const float* data; int h, w, stride; };

} // namespace

// ===========================================================================
//  构造 / 析构 / init
// ===========================================================================
Inferencer::Inferencer(const InferenceConfig& cfg) : cfg_(cfg) {}

Inferencer::~Inferencer() {
    if (ctx_ != 0) { rknn_destroy(ctx_); ctx_ = 0; }
}

bool Inferencer::init() {
    // 1. 初始化 RKNN 上下文（size=0 表示 model 参数为文件路径）。
    std::string path = cfg_.model_path;
    int ret = rknn_init(&ctx_, const_cast<char*>(path.c_str()), 0, 0, nullptr);
    if (ret < 0) { LOG_ERROR("rknn: init failed ret=%d", ret); return false; }

    // 2. 查询输入/输出数量。
    rknn_input_output_num io_num{};
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) { LOG_ERROR("rknn: query IO failed"); return false; }
    n_outputs_ = io_num.n_output;

    // 3. 查询输入尺寸。
    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (in_attr.fmt == RKNN_TENSOR_NCHW) {
        model_h_ = in_attr.dims[2]; model_w_ = in_attr.dims[3];
    } else {
        model_h_ = in_attr.dims[1]; model_w_ = in_attr.dims[2];
    }
    LOG_INFO("rknn: model input %ux%u, %u outputs", model_w_, model_h_, n_outputs_);

    // 4. 查询输出属性并分配缓冲。
    if (!queryOutputs()) return false;

    // 5. 加载标签。
    std::ifstream lf(cfg_.labels_path);
    if (lf.is_open()) {
        std::string line;
        while (std::getline(lf, line)) if (!line.empty()) labels_.push_back(line);
    }
    LOG_INFO("rknn: %zu labels loaded", labels_.size());

    rgb_buf_.resize(static_cast<size_t>(model_w_) * model_h_ * 3);
    inited_ = true;
    return true;
}

bool Inferencer::queryOutputs() {
    out_attrs_.resize(n_outputs_);
    out_offsets_.resize(n_outputs_);
    size_t total = 0;
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        out_attrs_[i].index = i;
        int ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR,
                             &out_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret < 0) { LOG_ERROR("rknn: output attr[%u] failed", i); return false; }
        int elems = 1;
        for (uint32_t d = 0; d < out_attrs_[i].n_dims; ++d) elems *= out_attrs_[i].dims[d];
        out_offsets_[i] = total;
        total += elems;
        LOG_INFO("rknn: output[%u] dims=[%u,%u,%u,%u] scale=%.4f zp=%d",
                 i, out_attrs_[i].dims[0], out_attrs_[i].dims[1],
                 out_attrs_[i].dims[2], out_attrs_[i].dims[3],
                 out_attrs_[i].scale, (int)out_attrs_[i].zp);
    }
    dequant_buf_.resize(total);
    return true;
}

// ===========================================================================
//  前处理：NV12 → RGB letterbox（优先 RGA 硬件加速，失败回退 CPU 浮点）
// ===========================================================================
void Inferencer::preprocess(const FramePtr& f, uint8_t* rgb, LetterboxInfo& lb) {
    const int w = f->width, h = f->height;

    float scale = std::min((float)model_w_ / w, (float)model_h_ / h);
    int sw = (int)(w * scale), sh = (int)(h * scale);
    lb.scale = scale;
    lb.pad_x = (model_w_ - sw) / 2;
    lb.pad_y = (model_h_ - sh) / 2;

    // 灰边填充（YOLOv5 惯例 114）。
    std::memset(rgb, 114, (size_t)model_w_ * model_h_ * 3);

    // RGA 硬件：NV12 → RGB 转换 + 等比缩放，再把结果拷到 letterbox 位置。
    if (rgaPreprocess(f->nv12.data(), w, h, sw, sh)) {
        for (int r = 0; r < sh; ++r) {
            std::memcpy(rgb + ((size_t)(r + lb.pad_y) * model_w_ + lb.pad_x) * 3,
                        rgb_tmp_.data() + (size_t)r * sw * 3,
                        (size_t)sw * 3);
        }
        return;
    }

    // 兜底：CPU 浮点转换（RGA 不可用时保证功能不坏）。
    const uint8_t* nv12 = f->nv12.data();
    const uint8_t* y_plane  = nv12;
    const uint8_t* uv_plane = nv12 + (size_t)w * h;

    for (int dy = 0; dy < sh; ++dy) {
        int sy = std::min((int)(dy / scale), h - 1);
        const uint8_t* y_row  = y_plane  + (size_t)sy * w;
        const uint8_t* uv_row = uv_plane + (size_t)(sy / 2) * w;
        uint8_t* dst = rgb + ((size_t)(dy + lb.pad_y) * model_w_ + lb.pad_x) * 3;
        for (int dx = 0; dx < sw; ++dx) {
            int sx = std::min((int)(dx / scale), w - 1);
            int yv = y_row[sx];
            size_t uv_off = (sx & ~1);
            int uv = uv_row[uv_off], vv = uv_row[uv_off + 1];
            float yf = (yv - 16) * 1.164f;
            float uf = uv - 128.0f, vf = vv - 128.0f;
            dst[dx * 3 + 0] = clamp8(yf + 1.596f * vf);
            dst[dx * 3 + 1] = clamp8(yf - 0.391f * uf - 0.813f * vf);
            dst[dx * 3 + 2] = clamp8(yf + 2.018f * uf);
        }
    }
}

// RGA 硬件做 NV12 → RGB 格式转换 + 等比缩放，结果写入 rgb_tmp_（sw x sh x 3）。
bool Inferencer::rgaPreprocess(const uint8_t* nv12, int w, int h, int sw, int sh) {
    if (rgb_tmp_.size() < (size_t)sw * sh * 3)
        rgb_tmp_.resize((size_t)sw * sh * 3);

    rga_buffer_t src = wrapbuffer_virtualaddr((void*)nv12, w, h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr((void*)rgb_tmp_.data(), sw, sh, RK_FORMAT_RGB_888);
    // fx=fy=0 表示按 dst/src 尺寸自动缩放；interpolation=0 用默认插值；sync=1 同步执行。
    IM_STATUS st = imresize(src, dst);
    if (st != IM_STATUS_SUCCESS) {
        LOG_WARN("inferencer: RGA preprocess failed: %s", imStrError(st));
        return false;
    }
    return true;
}

// ===========================================================================
//  detect：完整推理
// ===========================================================================
bool Inferencer::detect(const FramePtr& frame) {
    if (!inited_ || !frame) return false;
    uint64_t t0 = nowUs();

    // 1. 前处理。
    LetterboxInfo lb;
    preprocess(frame, rgb_buf_.data(), lb);
    uint64_t t1 = nowUs();

    // 2. 设置输入 + 推理。
    rknn_input in{};
    in.index = 0;
    in.type  = RKNN_TENSOR_UINT8;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.buf   = rgb_buf_.data();
    in.size  = static_cast<uint32_t>(rgb_buf_.size());
    if (rknn_inputs_set(ctx_, 1, &in) < 0) { LOG_ERROR("rknn: inputs_set failed"); return false; }
    if (rknn_run(ctx_, nullptr) < 0)         { LOG_ERROR("rknn: run failed"); return false; }

    // 3. 获取输出（INT8）。
    std::vector<rknn_output> outputs(n_outputs_);
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        outputs[i].index = i; outputs[i].want_float = 0; outputs[i].is_prealloc = 0;
    }
    if (rknn_outputs_get(ctx_, n_outputs_, outputs.data(), nullptr) < 0) {
        LOG_ERROR("rknn: outputs_get failed"); return false;
    }
    uint64_t t2 = nowUs();

    // 4. 反量化 → 构造输出头。
    std::vector<OutputTensor> tensors;
    tensors.reserve(n_outputs_);
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        const rknn_tensor_attr& a = out_attrs_[i];
        int8_t* src = static_cast<int8_t*>(outputs[i].buf);
        float*  dst = dequant_buf_.data() + out_offsets_[i];
        int elems = 1;
        for (uint32_t d = 0; d < a.n_dims; ++d) elems *= a.dims[d];
        for (int e = 0; e < elems; ++e) dst[e] = dequant(src[e], a.zp, a.scale);
        int gw = a.dims[3], gh = a.dims[2];
        tensors.push_back({dst, gh, gw, static_cast<int>(model_w_ / gw)});
    }

    // 5. YOLOv5 解码 + NMS（输出原图坐标系检测框）。
    const int img_w = frame->width, img_h = frame->height;
    DetectResult result{};
    std::vector<Candidate> cands;

    for (size_t o = 0; o < tensors.size() && o < 3; ++o) {
        const OutputTensor& t = tensors[o];
        const int glen = t.h * t.w;
        const int anchor_row = (t.stride == 8) ? 0 : (t.stride == 16) ? 1 : 2;
        for (int a = 0; a < kNumAnchors; ++a) {
            const int base = kBoxChannels * a;   // 该 anchor 的通道基址
            for (int gy = 0; gy < t.h; ++gy) {
                for (int gx = 0; gx < t.w; ++gx) {
                    const int idx = gy * t.w + gx;
                    const int off = base * glen + idx;   // NCHW 数据基偏移

                    float obj = sigmoid(t.data[off + 4 * glen]);
                    if (obj < cfg_.conf_threshold) continue;

                    int   max_cls = 0;
                    float max_val = t.data[off + 5 * glen];
                    for (int c = 1; c < kNumClasses; ++c) {
                        float v = t.data[off + (5 + c) * glen];
                        if (v > max_val) { max_val = v; max_cls = c; }
                    }
                    float score = obj * sigmoid(max_val);
                    if (score < cfg_.conf_threshold) continue;

                    float tx = sigmoid(t.data[off + 0 * glen]);
                    float ty = sigmoid(t.data[off + 1 * glen]);
                    float tw = sigmoid(t.data[off + 2 * glen]);
                    float th = sigmoid(t.data[off + 3 * glen]);
                    float bx = (tx * 2.0f - 0.5f + gx) * t.stride;
                    float by = (ty * 2.0f - 0.5f + gy) * t.stride;
                    float bw = std::pow(tw * 2.0f, 2) * kAnchors[anchor_row][2 * a];
                    float bh = std::pow(th * 2.0f, 2) * kAnchors[anchor_row][2 * a + 1];

                    // 模型坐标(640×640) → 原图坐标：减 padding 除 scale。
                    cands.push_back({
                        (bx - bw * 0.5f - lb.pad_x) / lb.scale,
                        (by - bh * 0.5f - lb.pad_y) / lb.scale,
                        bw / lb.scale, bh / lb.scale, score, max_cls});
                }
            }
        }
    }

    // NMS：按类别抑制重叠框。
    std::vector<char> keep(cands.size(), 1);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (!keep[j] || cands[i].cls != cands[j].cls) continue;
            if (boxIoU(cands[i].x, cands[i].y, cands[i].w, cands[i].h,
                       cands[j].x, cands[j].y, cands[j].w, cands[j].h) > cfg_.nms_threshold)
                keep[j] = 0;
        }
    }

    // 输出结果：钳制边界 + 填标签。
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
        if (c.cls >= 0 && static_cast<size_t>(c.cls) < labels_.size())
            std::snprintf(b.label, sizeof(b.label), "%s", labels_[c.cls].c_str());
        else
            std::snprintf(b.label, sizeof(b.label), "cls_%d", c.cls);
        ++result.count;
    }

    frame->detect = result;
    frame->inference_ts = nowUs();

    uint64_t t3 = nowUs();
    static int dbg = 0;
    if (++dbg % 25 == 0)
        LOG_INFO("detect: pre=%llu rknn=%llu post=%llu us",
                 (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
                 (unsigned long long)(t3 - t2));

    rknn_outputs_release(ctx_, n_outputs_, outputs.data());
    return true;
}

// ===========================================================================
//  NV12 画框（编码线程在编码前调用）
// ===========================================================================
void drawBoxesNv12(uint8_t* nv12, int w, int h, const DetectResult& det) {
    uint8_t* y = nv12;
    for (uint32_t i = 0; i < det.count; ++i) {
        const DetectBox& b = det.boxes[i];
        int x0 = std::max(0, std::min(b.x, w - 1));
        int y0 = std::max(0, std::min(b.y, h - 1));
        int x1 = std::max(0, std::min(b.x + b.w - 1, w - 1));
        int y1 = std::max(0, std::min(b.y + b.h - 1, h - 1));
        if (x1 < x0 || y1 < y0) continue;
        for (int x = x0; x <= x1; ++x) {
            y[(size_t)y0 * w + x] = 255;
            y[(size_t)y1 * w + x] = 255;
        }
        for (int yy = y0; yy <= y1; ++yy) {
            y[(size_t)yy * w + x0] = 255;
            y[(size_t)yy * w + x1] = 255;
        }
    }
}

} // namespace vision
