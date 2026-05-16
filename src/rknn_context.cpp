#include "rknn_context.hpp"
#include "logger.hpp"

#ifdef X86_DEBUG

// x86 stub: RKNN SDK不可用，所有方法返回false
namespace rk3568_vision {

RknnContext::~RknnContext() { ctx_ = 0; }
bool RknnContext::init(const std::string&, uint32_t) {
    LOG_WARN("RKNN not available on x86 — inference disabled");
    return false;
}
bool RknnContext::set_inputs(const rknn_input*, uint32_t) { return false; }
bool RknnContext::run() { return false; }
bool RknnContext::get_outputs(rknn_output*, uint32_t) { return false; }
bool RknnContext::release_outputs(rknn_output*, uint32_t) { return false; }

} // namespace rk3568_vision

#else

// Real RKNN implementation for ARM/RK3568
#include <cstdio>

namespace rk3568_vision {

static std::vector<uint8_t> load_file(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) { LOG_ERROR("Cannot open model: %s", path.c_str()); return {}; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, fp);
    fclose(fp);
    LOG_INFO("Model loaded: %s (%ld bytes)", path.c_str(), size);
    return data;
}

RknnContext::~RknnContext() {
    if (ctx_ != 0) { rknn_destroy(ctx_); ctx_ = 0; }
}

bool RknnContext::init(const std::string& model_path, uint32_t npu_core) {
    model_data_ = load_file(model_path);
    if (model_data_.empty()) return false;

    int ret = rknn_init(&ctx_, model_data_.data(), model_data_.size(), 0, nullptr);
    if (ret < 0) { LOG_ERROR("rknn_init failed: ret=%d", ret); return false; }

    rknn_set_core_mask(ctx_, (npu_core == 0) ? RKNN_NPU_CORE_0
                          : (npu_core == 1) ? RKNN_NPU_CORE_1 : RKNN_NPU_CORE_2);

    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0) { LOG_ERROR("rknn_query IO failed"); return false; }
    output_count_ = io_num_.n_output;

    LOG_INFO("RKNN model IO: %u inputs, %u outputs", io_num_.n_input, io_num_.n_output);

    input_attrs_.reset(static_cast<rknn_tensor_attr*>(
        calloc(io_num_.n_input, sizeof(rknn_tensor_attr))));
    for (uint32_t i = 0; i < io_num_.n_input; ++i) {
        input_attrs_.get()[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR,
                          &input_attrs_.get()[i], sizeof(rknn_tensor_attr));
        if (ret < 0) { LOG_ERROR("input attr[%u] failed", i); return false; }
    }

    output_attrs_.reset(static_cast<rknn_tensor_attr*>(
        calloc(io_num_.n_output, sizeof(rknn_tensor_attr))));
    for (uint32_t i = 0; i < io_num_.n_output; ++i) {
        output_attrs_.get()[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR,
                   &output_attrs_.get()[i], sizeof(rknn_tensor_attr));
    }

    auto& attr = input_attrs_.get()[0];
    if (attr.fmt == RKNN_TENSOR_NCHW) {
        input_channel_ = attr.dims[1]; input_height_ = attr.dims[2]; input_width_ = attr.dims[3];
    } else {
        input_height_ = attr.dims[1]; input_width_ = attr.dims[2]; input_channel_ = attr.dims[3];
    }
    LOG_INFO("Model input: %ux%ux%u", input_width_, input_height_, input_channel_);
    return true;
}

bool RknnContext::set_inputs(const rknn_input* inputs, uint32_t n_input) {
    return rknn_inputs_set(ctx_, n_input, const_cast<rknn_input*>(inputs)) >= 0;
}

bool RknnContext::run() { return rknn_run(ctx_, nullptr) >= 0; }

bool RknnContext::get_outputs(rknn_output* outputs, uint32_t n_output) {
    return rknn_outputs_get(ctx_, n_output, outputs, nullptr) >= 0;
}

bool RknnContext::release_outputs(rknn_output* outputs, uint32_t n_output) {
    return rknn_outputs_release(ctx_, n_output, outputs) >= 0;
}

} // namespace rk3568_vision

#endif
