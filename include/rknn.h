/*
 * ==========================================================================
 * rknn.h — RKNN Runtime API 封装（C 语言接口头文件）
 * ==========================================================================
 *
 * **为什么需要这个头文件？**
 *   - 对 RKNN SDK 原生 API 做轻量封装，提供统一的 C 接口
 *   - 支持跨平台编译：RK3568 使用真实 SDK，x86 使用 stub 定义
 *
 * **跨平台策略**：
 *   - X86_DEBUG 定义时：提供 rknn 结构体和宏的 stub 定义，代码可编译但不执行推理
 *   - 非 X86_DEBUG（即 RK3568 上）：直接 include "rknn_api.h"（Rockchip 官方 SDK 头文件）
 *   - CMakeLists.txt 中自动检测平台：aarch64 不定义 X86_DEBUG，x86_64 定义 X86_DEBUG
 *
 * **API 设计**：
 *   所有函数使用 rknn_ctx_* 命名前缀，封装了原始 rknn_* API
 *   提供清晰的错误处理和返回值检查
 *   rknn_ctx_t 是不透明指针（opaque pointer），隐藏内部实现细节
 */

#ifndef RKNN_H
#define RKNN_H

#include <stdint.h>
#include <stdbool.h>

/* ── 跨平台兼容 ──────────────────────────────────────────────────────── */
#ifdef X86_DEBUG
/* x86 stub 定义：提供与 RKNN SDK 兼容的结构体和宏 */
typedef struct { int32_t scale, zp; int32_t dims[4], fmt, type, n_dims; char name[64]; } rknn_tensor_attr;
typedef struct { void* buf; uint32_t size; uint8_t want_float, is_prealloc; int32_t index; } rknn_output;
typedef struct { void* buf; uint32_t type, size, fmt; uint8_t pass_through; int32_t index; } rknn_input;
typedef void* rknn_context;
#define RKNN_TENSOR_INT8  0    /* INT8 量化张量类型    */
#define RKNN_TENSOR_UINT8 2    /* UINT8 未量化张量类型 */
#define RKNN_TENSOR_NHWC  1    /* NHWC 内存布局       */
#define RKNN_TENSOR_NCHW  0    /* NCHW 内存布局       */
#else
#include "rknn_api.h"           /* Rockchip 官方 SDK 头文件 */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄：隐藏内部实现细节 */
typedef struct rknn_ctx_s rknn_ctx_t;

/*
 * 打开 RKNN 模型上下文（加载模型 + 初始化 NPU）
 *
 * @model_path：.rknn 模型文件路径
 * @npu_core：NPU 核心编号（RK3568 为 0）
 * 返回：rknn_ctx_t*（不透明句柄），失败返回 NULL
 */
rknn_ctx_t* rknn_ctx_open(const char* model_path, uint32_t npu_core);

/* 关闭 RKNN 上下文并释放所有资源 */
void        rknn_ctx_close(rknn_ctx_t* ctx);

/* 设置 NPU 输入数据 */
int         rknn_ctx_set_inputs(rknn_ctx_t* ctx, const rknn_input* inputs, uint32_t n);

/* 执行 NPU 推理 */
int         rknn_ctx_run(rknn_ctx_t* ctx);

/* 获取 NPU 输出（推理结果） */
int         rknn_ctx_get_outputs(rknn_ctx_t* ctx, rknn_output* outputs, uint32_t n);

/* 释放输出缓冲区（每次 get_outputs 后必须调用） */
int         rknn_ctx_release_outputs(rknn_ctx_t* ctx, rknn_output* outputs, uint32_t n);

/* ── 属性查询 ──────────────────────────────────────────────────────── */
uint32_t    rknn_ctx_input_w(rknn_ctx_t* ctx);       /* 模型输入宽度        */
uint32_t    rknn_ctx_input_h(rknn_ctx_t* ctx);       /* 模型输入高度        */
uint32_t    rknn_ctx_input_c(rknn_ctx_t* ctx);       /* 模型输入通道数      */
uint32_t    rknn_ctx_output_count(rknn_ctx_t* ctx);  /* 模型输出头数量      */
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* ctx, uint32_t idx); /* 输出张量属性 */

#ifdef __cplusplus
}
#endif
#endif /* RKNN_H */
