/**
 * ==========================================================================
 * rknn_context.hpp — RKNN 模型上下文 RAII 封装头文件
 * ==========================================================================
 *
 * **RknnContext 类**：RAII 风格的 RKNN 模型管理
 *   自动管理：模型内存加载、rknn_context 创建/销毁、张量属性查询
 *   使用 std::unique_ptr 管理属性数组，析构时自动释放
 *
 * **为什么用 RAII？**
 *   - 避免忘记 rknn_destroy() 导致 NPU 资源泄漏
 *   - 异常安全（即使抛出异常也能正确清理）
 *   - 移动语义（std::unique_ptr）防止双重释放
 */

#pragma once

#ifdef X86_DEBUG
/* x86 stub 类型：模拟 RKNN 结构体 */
#include <cstdint>
typedef struct { int32_t scale; int32_t zp; int32_t dims[4]; int32_t fmt; int32_t type; int32_t n_dims; char name[64]; } rknn_tensor_attr;
typedef struct { void* buf; uint32_t size; uint8_t want_float; uint8_t is_prealloc; int32_t index; } rknn_output;
typedef struct { void* buf; uint32_t type; uint32_t size; uint32_t fmt; uint8_t pass_through; int32_t index; } rknn_input;
typedef struct { uint32_t n_input; uint32_t n_output; } rknn_input_output_num;
typedef void* rknn_context;
#define RKNN_TENSOR_INT8   0
#define RKNN_TENSOR_UINT8  2
#define RKNN_TENSOR_NHWC   1
#define RKNN_TENSOR_NCHW   0
#else
#include "rknn_api.h"    /* Rockchip 官方 RKNN SDK 头文件 */
#endif

#include <string>
#include <vector>
#include <memory>
#include <atomic>

namespace rk3568_vision {

class RknnContext {
public:
    RknnContext() = default;
    ~RknnContext();

    /* 禁止拷贝（防止双重释放 rknn_context） */
    RknnContext(const RknnContext&) = delete; // 等号后面的 = delete 是一个整体语法标记，用于显式禁用该函数。
    RknnContext& operator=(const RknnContext&) = delete;

    /* 初始化：加载模型 + 创建上下文 + 查询张量属性 */
    bool init(const std::string& model_path, uint32_t npu_core = 0);

    /* 推理三步曲：set_inputs → run → get_outputs → release_outputs */
    bool set_inputs(const rknn_input* inputs, uint32_t n_input);
    bool run();
    bool get_outputs(rknn_output* outputs, uint32_t n_output);
    bool release_outputs(rknn_output* outputs, uint32_t n_output);

    /* 获取原生 rknn_context 句柄（用于高级操作） */
    rknn_context ctx() const { return ctx_; }

    /* 属性查询 */
    uint32_t input_width()   const { return input_width_; }
    uint32_t input_height()  const { return input_height_; }
    uint32_t input_channel() const { return input_channel_; }
    uint32_t output_count()  const { return output_count_; }

    /* 根据索引获取输出张量属性（用于后处理） */
    const rknn_tensor_attr& output_attr(int idx) const { return output_attrs_.get()[idx]; }

private:
    rknn_context  ctx_ = 0;                    /* RKNN 运行时句柄 */
    std::vector<uint8_t> model_data_;           /* 加载到内存的模型数据 */

    uint32_t input_width_   = 640;              /* 模型输入宽度 */
    uint32_t input_height_  = 640;              /* 模型输入高度 */
    uint32_t input_channel_ = 3;                /* 模型输入通道数 */
    uint32_t output_count_  = 0;                /* 输出头数量 */
    rknn_input_output_num io_num_{0, 0};       /* 输入/输出数量 */

    /* 自定义 Deleter：用 free 释放 calloc 分配的内存 */
    struct AttrDeleter { 
        void operator()(rknn_tensor_attr* p) 
        { free(p); } 
    };
    // 重载了 operator()，使其可以像函数一样调用
    // std::unique_ptr 在析构时会调用 deleter(p)   
    
    std::unique_ptr<rknn_tensor_attr, AttrDeleter> input_attrs_;   /* 输入属性 */
    std::unique_ptr<rknn_tensor_attr, AttrDeleter> output_attrs_;  /* 输出属性 */
};

} // namespace rk3568_vision
