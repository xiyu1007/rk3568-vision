#pragma once

#ifdef X86_DEBUG
// x86 stub types — 模拟RKNN结构体，使代码在x86上可通过编译
#include <cstdint>
typedef struct { int32_t scale; int32_t zp; int32_t dims[4]; int32_t fmt; int32_t type; int32_t n_dims; char name[64]; } rknn_tensor_attr;
typedef struct { void* buf; uint32_t size; uint8_t want_float; uint8_t is_prealloc; int32_t index; } rknn_output;
typedef struct { void* buf; uint32_t type; uint32_t size; uint32_t fmt; uint8_t pass_through; int32_t index; } rknn_input;
typedef struct { uint32_t n_input; uint32_t n_output; } rknn_input_output_num;
typedef void* rknn_context;
#define RKNN_TENSOR_UINT8  2
#define RKNN_TENSOR_NHWC   1
#define RKNN_TENSOR_NCHW   0
#else
#include "rknn_api.h"
#endif

#include <string>
#include <vector>
#include <memory>
#include <atomic>

namespace rk3568_vision {

// ============================================================================
// RknnContext — RKNN 模型上下文的 RAII 封装
//
// 生命周期:
//   1. 构造: 加载模型文件到内存
//   2. init(): 创建 rknn_context，查询输入/输出属性
//   3. 推理: set_inputs() -> run() -> get_outputs()
//   4. 析构: 释放 context、模型数据、属性数组
//
// 为什么用 RAII:
//   - 原代码使用裸 malloc/free + rknn_destroy，容易忘记释放
//   - std::unique_ptr 管理模型数据和属性，析构自动清理
// ============================================================================
class RknnContext {
public:
    RknnContext() = default;
    ~RknnContext();

    RknnContext(const RknnContext&) = delete;
    RknnContext& operator=(const RknnContext&) = delete;

    bool init(const std::string& model_path, uint32_t npu_core = 0);
    bool set_inputs(const rknn_input* inputs, uint32_t n_input);
    bool run();
    bool get_outputs(rknn_output* outputs, uint32_t n_output);
    bool release_outputs(rknn_output* outputs, uint32_t n_output);

    rknn_context ctx() const { return ctx_; }

    uint32_t input_width()   const { return input_width_; }
    uint32_t input_height()  const { return input_height_; }
    uint32_t input_channel() const { return input_channel_; }
    uint32_t output_count()  const { return output_count_; }

    const rknn_tensor_attr& output_attr(int idx) const { return output_attrs_.get()[idx]; }

private:
    rknn_context  ctx_ = 0;
    std::vector<uint8_t> model_data_;

    uint32_t input_width_   = 640;
    uint32_t input_height_  = 640;
    uint32_t input_channel_ = 3;
    uint32_t output_count_  = 0;
    rknn_input_output_num io_num_{0, 0};

    struct AttrDeleter { void operator()(rknn_tensor_attr* p) { free(p); } };
    std::unique_ptr<rknn_tensor_attr, AttrDeleter> input_attrs_;
    std::unique_ptr<rknn_tensor_attr, AttrDeleter> output_attrs_;
};

} // namespace rk3568_vision
