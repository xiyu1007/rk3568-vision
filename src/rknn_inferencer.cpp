// ============================================================================
// rknn_inferencer.cpp — 真实 RKNN 推理实现
// ============================================================================

#ifdef VISION_RK3568

#include "rknn_inferencer.hpp"

#include <fstream>

#include "logger.hpp"

namespace vision {

namespace {
// 反量化：int8 → float。公式 float_val = (int8_val - zp) * scale。
inline float dequant(int8_t v, int32_t zp, float scale) {
    return (static_cast<float>(v) - static_cast<float>(zp)) * scale;
}
} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
RknnInferencer::RknnInferencer(const Config& cfg) : cfg_(cfg) {}

RknnInferencer::~RknnInferencer() {
    if (ctx_ != 0) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
}

// ---------------------------------------------------------------------------
// init：加载模型 + 查询属性 + 加载标签
// ---------------------------------------------------------------------------
bool RknnInferencer::init() {
    // 1. 用路径方式初始化 RKNN 上下文（size=0 表示 model 参数是文件路径）。
    std::string path = cfg_.inference.model_path;
    int ret = rknn_init(&ctx_, const_cast<char*>(path.c_str()), 0, 0, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn: init failed, ret=%d", ret);
        return false;
    }

    // 2. 查询输入/输出数量。
    rknn_input_output_num io_num{};
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        LOG_ERROR("rknn: query IO num failed");
        return false;
    }
    n_outputs_ = io_num.n_output;
    LOG_INFO("rknn: %u inputs, %u outputs", io_num.n_input, io_num.n_output);

    // 3. 查询输入属性，得到模型输入尺寸。
    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (ret < 0) {
        LOG_ERROR("rknn: query input attr failed");
        return false;
    }
    // NHWC: dims[1]=H, dims[2]=W；NCHW: dims[2]=H, dims[3]=W。
    if (in_attr.fmt == RKNN_TENSOR_NCHW) {
        model_h_ = in_attr.dims[2];
        model_w_ = in_attr.dims[3];
    } else {
        model_h_ = in_attr.dims[1];
        model_w_ = in_attr.dims[2];
    }
    LOG_INFO("rknn: model input %ux%u fmt=%d", model_w_, model_h_, (int)in_attr.fmt);

    // 4. 查询输出属性并初始化反量化缓冲。
    if (!queryOutputs()) return false;

    // 5. 加载类别标签。
    std::ifstream lf(cfg_.inference.labels_path);
    if (lf.is_open()) {
        std::string line;
        while (std::getline(lf, line)) {
            if (!line.empty()) labels_.push_back(line);
        }
    }
    LOG_INFO("rknn: %zu labels loaded", labels_.size());

    // 6. 预分配前处理 RGB 缓冲。
    rgb_buf_.resize(static_cast<size_t>(model_w_) * model_h_ * 3);

    // 7. 创建后处理解码器。
    decoder_ = std::make_unique<YoloDecoder>(
        cfg_.inference.conf_threshold, cfg_.inference.nms_threshold, labels_);

    inited_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// queryOutputs：查询各输出张量属性并计算反量化缓冲布局
// ---------------------------------------------------------------------------
bool RknnInferencer::queryOutputs() {
    out_attrs_.resize(n_outputs_);
    out_offsets_.resize(n_outputs_);

    size_t total = 0;
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        out_attrs_[i].index = i;
        int ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR,
                             &out_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret < 0) {
            LOG_ERROR("rknn: query output attr[%u] failed", i);
            return false;
        }
        // 元素个数 = 各维度乘积。
        int elems = 1;
        for (uint32_t d = 0; d < out_attrs_[i].n_dims; ++d) {
            elems *= out_attrs_[i].dims[d];
        }
        out_offsets_[i] = total;
        total += elems;

        LOG_INFO("rknn: output[%u] dims=[%u,%u,%u,%u] scale=%.4f zp=%d fmt=%d",
                 i, out_attrs_[i].dims[0], out_attrs_[i].dims[1],
                 out_attrs_[i].dims[2], out_attrs_[i].dims[3],
                 out_attrs_[i].scale, (int)out_attrs_[i].zp,
                 (int)out_attrs_[i].fmt);
    }
    dequant_buf_.resize(total);
    return true;
}

// ---------------------------------------------------------------------------
// detect：对一帧执行检测
// ---------------------------------------------------------------------------
bool RknnInferencer::detect(const FramePtr& frame) {
    if (!inited_ || !frame) return false;

    // ---- 前处理：NV12 → RGB + letterbox ----
    LetterboxInfo lb;
    nv12ToRgbLetterbox(frame->nv12.data(), frame->width, frame->height,
                       rgb_buf_.data(), model_w_, model_h_, lb);

    // ---- 设置输入 ----
    rknn_input in{};
    in.index        = 0;
    in.type         = RKNN_TENSOR_UINT8;
    in.fmt          = RKNN_TENSOR_NHWC;   // 交错的 RGB888
    in.buf          = rgb_buf_.data();
    in.size         = static_cast<uint32_t>(rgb_buf_.size());
    in.pass_through = 0;
    if (rknn_inputs_set(ctx_, 1, &in) < 0) {
        LOG_ERROR("rknn: inputs_set failed");
        return false;
    }

    // ---- 推理（性能关键路径：~25ms）----
    if (rknn_run(ctx_, nullptr) < 0) {
        LOG_ERROR("rknn: run failed");
        return false;
    }

    // ---- 获取输出（INT8 原始数据）----
    std::vector<rknn_output> outputs(n_outputs_);
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        outputs[i].index      = i;
        outputs[i].want_float = 0;   // 拿 INT8，CPU 侧反量化
        outputs[i].is_prealloc = 0;
    }
    if (rknn_outputs_get(ctx_, n_outputs_, outputs.data(), nullptr) < 0) {
        LOG_ERROR("rknn: outputs_get failed");
        return false;
    }

    // ---- 反量化 + 构造解码器输入 ----
    std::vector<OutputTensor> tensors;
    tensors.reserve(n_outputs_);
    for (uint32_t i = 0; i < n_outputs_; ++i) {
        const rknn_tensor_attr& a = out_attrs_[i];
        int8_t* src = static_cast<int8_t*>(outputs[i].buf);
        float*  dst = dequant_buf_.data() + out_offsets_[i];

        // 元素总数。
        int elems = 1;
        for (uint32_t d = 0; d < a.n_dims; ++d) elems *= a.dims[d];
        for (int e = 0; e < elems; ++e) {
            dst[e] = dequant(src[e], a.zp, a.scale);
        }

        // NCHW [1, 255, H, W]：网格 H=W 在 dims[2]/dims[3]。
        int gw = a.dims[3];
        int gh = a.dims[2];
        int stride = model_w_ / gw;   // 640/80=8, 640/40=16, 640/20=32
        tensors.push_back({dst, gh, gw, stride});
    }

    // ---- 后处理：解码 + NMS，结果写回 frame->detect ----
    frame->detect = decoder_->decode(tensors, lb, frame->width, frame->height);
    frame->inference_ts = nowUs();

    // ---- 释放输出 ----
    rknn_outputs_release(ctx_, n_outputs_, outputs.data());
    return true;
}

} // namespace vision

#endif // VISION_RK3568
