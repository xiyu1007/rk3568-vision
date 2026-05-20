/*
 * ==========================================================================
 * rknn.c — RKNN Runtime API 封装（C 语言接口）
 * ==========================================================================
 *
 * **功能**：封装 Rockchip RKNN SDK 的 C API，提供模型加载/推理/输出获取功能
 *
 * **为什么需要两层封装（rknn.c + rknn_context.cpp）？**
 *   - rknn.c：轻量 C 封装，直接调用 rknn_api.h 中的 C 函数
 *   - rknn_context.cpp：C++ RAII 封装，自动管理模型内存
 *   - 两个封装层服务于不同场景：C 代码用 rknn.c，C++ Detector 用 rknn_context.cpp
 *   - 实际项目中 Detector（C++）使用 rknn_context.cpp 保持 RAII 风格
 *   - rknn.c 作为备选，提供简洁的 C 接口
 *
 * **RKNN 推理流程（标准三步）**：
 *   1. rknn_init() — 加载模型、初始化 NPU 运行时
 *   2. rknn_query() — 查询输入/输出张量属性（形状、格式、量化参数）
 *   3. 推理循环：rknn_inputs_set() → rknn_run() → rknn_outputs_get()
 *
 * **NPU 硬件加速原理**：
 *   - RK3568 内置 1 TOPS（Tera Operations Per Second）算力的 NPU
 *   - INT8 量化推理（8-bit 整数运算），比 FP32 推理快 4× 且省电
 *   - YOLOv5s 模型经过 RKNN Toolkit 转换为 INT8 格式 (.rknn)
 *   - rknn_run() 将计算任务提交给 NPU，CPU 可同时处理其他任务
 *   - NPU 推理期间 CPU 占用几乎为零，实现真正硬件并行
 *
 * **跨平台兼容（X86_DEBUG 宏）**：
 *   - RK3568 上：链接真实的 librknnrt.so，正常调用 NPU
 *   - x86 开发机上：所有函数返回空实现/stub，保证代码可编译
 *   - 这样可以在 PC 上进行语法检查和逻辑验证，无需实体板卡
 */

#ifdef X86_DEBUG

/* ==========================================================================
 *  x86 调试桩（Stub）实现
 *  所有函数不执行实际推理，仅在日志中提示 "RKNN not available on x86"
 *  没有 NPU 硬件，无法进行真正的推理
 * ========================================================================== */

#include "rknn.h"
#include "logger.h"
#include <stdlib.h>

struct rknn_ctx_s { int stub; };

rknn_ctx_t* rknn_ctx_open(const char* path, uint32_t core) {
    (void)path; (void)core;
    LOG_WARN("RKNN not available on x86");
    return NULL;
}
void rknn_ctx_close(rknn_ctx_t* c) { free(c); }
int rknn_ctx_set_inputs(rknn_ctx_t* c, const rknn_input* i, uint32_t n) { (void)c;(void)i;(void)n; return 0; }
int rknn_ctx_run(rknn_ctx_t* c) { (void)c; return 0; }
int rknn_ctx_get_outputs(rknn_ctx_t* c, rknn_output* o, uint32_t n) { (void)c;(void)o;(void)n; return 0; }
int rknn_ctx_release_outputs(rknn_ctx_t* c, rknn_output* o, uint32_t n) { (void)c;(void)o;(void)n; return 0; }
uint32_t rknn_ctx_input_w(rknn_ctx_t* c)  { (void)c; return 640; }
uint32_t rknn_ctx_input_h(rknn_ctx_t* c)  { (void)c; return 640; }
uint32_t rknn_ctx_input_c(rknn_ctx_t* c)  { (void)c; return 3; }
uint32_t rknn_ctx_output_count(rknn_ctx_t* c) { (void)c; return 3; }
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* c, uint32_t i) { (void)c;(void)i; return NULL; }

#else

/* ==========================================================================
 *  ARM64 真实 RKNN 实现（运行在 RK3568 上）
 * ========================================================================== */

#include "rknn.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * RKNN 上下文结构体（不透明句柄）
 * 持有模型数据、rknn_context、输入/输出张量属性等所有状态
 */
struct rknn_ctx_s {
    rknn_context       ctx;           /* RKNN SDK 运行时句柄                   */
    uint8_t*           model_data;    /* 加载到内存的 .rknn 模型文件二进制数据 */
    size_t             model_size;    /* 模型文件大小（字节）                   */
    uint32_t           in_w, in_h, in_c;  /* 模型输入尺寸（宽/高/通道数）      */
    uint32_t           out_n;         /* 输出张量数量（YOLOv5 为 3 个输出头）   */
    rknn_input_output_num io_num;     /* 输入/输出数量                          */
    rknn_tensor_attr*  input_attrs;   /* 输入张量属性数组                      */
    rknn_tensor_attr*  output_attrs;  /* 输出张量属性数组                      */
};

/*
 * 加载模型文件到内存
 *
 * 为什么一次性加载到内存而不是流式读取？
 *   - 模型文件很小（YOLOv5s INT8 约 7MB）
 *   - rknn_init() 需要完整的模型二进制数据
 *   - 一次性加载到堆内存，避免后续文件 I/O
 *   - 模型数据在推理期间会一直被 RKNN SDK 内部引用
 *
 * @path：模型文件路径（如 "model/yolov5s.rknn"）
 * @out_sz：输出参数，返回文件大小
 * 返回：malloc 分配的模型数据缓冲区（调用者负责释放），失败返回 NULL
 */
static uint8_t* load_model_file(const char* path, size_t* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG_ERROR("cannot open model: %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(sz);
    if (data && fread(data, 1, sz, f) != (size_t)sz) {
        free(data); fclose(f);
        return NULL;
    }
    fclose(f);
    *out_sz = (size_t)sz;
    LOG_INFO("model loaded: %s (%ld bytes)", path, sz);
    return data;
}

/*
 * 打开 RKNN 模型上下文
 *
 * 初始化流程：
 *   1. 加载 .rknn 模型文件到内存
 *   2. rknn_init()：创建运行时上下文，解析模型结构
 *   3. rknn_set_core_mask()：指定使用哪个 NPU 核心（RK3568 仅核心 0）
 *   4. rknn_query(RKNN_QUERY_IN_OUT_NUM)：查询输入/输出张量数量
 *   5. rknn_query(RKNN_QUERY_INPUT_ATTR)：查询每个输入张量的属性
 *   6. rknn_query(RKNN_QUERY_OUTPUT_ATTR)：查询每个输出张量的属性
 *   7. 从输入属性中解析模型输入尺寸（in_w, in_h, in_c）
 *
 * YOLOv5s 输出张量属性示例：
 *   [0] dims=[1,255,80,80]   stride=8  检测小目标
 *   [1] dims=[1,255,40,40]   stride=16 检测中目标
 *   [2] dims=[1,255,20,20]   stride=32 检测大目标
 *   其中 255 = 3 anchors × 85 (x,y,w,h,obj_conf + 80 classes)
 *
 * @model_path：.rknn 模型文件路径
 * @npu_core：NPU 核心号（0=核心0, 1=核心1, 2=核心2）
 * 返回：rknn_ctx_t*（失败返回 NULL）
 */
rknn_ctx_t* rknn_ctx_open(const char* model_path, uint32_t npu_core) {
    rknn_ctx_t* c = calloc(1, sizeof(rknn_ctx_t));
    if (!c) return NULL;

    /* 1. 加载模型文件到内存 */
    c->model_data = load_model_file(model_path, &c->model_size);
    if (!c->model_data) { free(c); return NULL; }

    /*
     * 2. 初始化 RKNN 运行时
     * rknn_init 参数：
     *   - &c->ctx：输出参数，返回运行时句柄
     *   - c->model_data：模型二进制数据
     *   - c->model_size：模型数据大小
     *   - 0：标志位（通常为 0，不使用特殊标志）
     *   - NULL：扩展参数（不使用）
     */
    int ret = rknn_init(&c->ctx, c->model_data, c->model_size, 0, NULL);
    if (ret < 0) { LOG_ERROR("rknn_init failed: %d", ret); free(c->model_data); free(c); return NULL; }

    /*
     * 3. 设置 NPU 核心
     * RK3568 有单一 NPU 核心（RKNN_NPU_CORE_0）
     * 多核 RK3588 才需要选择其他核心
     */
    rknn_set_core_mask(c->ctx, npu_core == 0 ? RKNN_NPU_CORE_0 :
                                npu_core == 1 ? RKNN_NPU_CORE_1 : RKNN_NPU_CORE_2);

    /* 4. 查询输入/输出张量数量 */
    ret = rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &c->io_num, sizeof(c->io_num));
    if (ret < 0) { LOG_ERROR("rknn_query IO failed"); rknn_ctx_close(c); return NULL; }
    c->out_n = c->io_num.n_output;
    LOG_INFO("RKNN IO: %u in, %u out", c->io_num.n_input, c->io_num.n_output);

    /*
     * 5. 查询输入张量属性
     * YOLOv5s 只有 1 个输入张量（RGB 图像 640×640×3）
     * 属性包括：形状、数据类型、量化参数等
     */
    c->input_attrs = calloc(c->io_num.n_input, sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < c->io_num.n_input; i++) {
        c->input_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_INPUT_ATTR, &c->input_attrs[i], sizeof(rknn_tensor_attr));
    }

    /*
     * 6. 查询输出张量属性
     * YOLOv5s 有 3 个输出张量（对应三种 stride 的检测头）
     */
    c->output_attrs = calloc(c->io_num.n_output, sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < c->io_num.n_output; i++) {
        c->output_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &c->output_attrs[i], sizeof(rknn_tensor_attr));
    }

    /*
     * 7. 解析模型输入尺寸
     * 支持 NCHW 和 NHWC 两种维度排列：
     *   NCHW: dims = [batch, channels, height, width]
     *   NHWC: dims = [batch, height, width, channels]
     * YOLOv5s 通常使用 NCHW 格式
     */
    rknn_tensor_attr* a = &c->input_attrs[0];
    if (a->fmt == RKNN_TENSOR_NCHW) {
        c->in_c = a->dims[1]; c->in_h = a->dims[2]; c->in_w = a->dims[3];
    } else {
        /* NHWC: 通道数在最后一维 */
        c->in_h = a->dims[1]; c->in_w = a->dims[2]; c->in_c = a->dims[3];
    }
    LOG_INFO("model input: %ux%ux%u", c->in_w, c->in_h, c->in_c);
    return c;
}

/*
 * 关闭 RKNN 上下文并释放所有资源
 * 顺序：销毁运行时 → 释放模型数据 → 释放属性数组 → 释放结构体
 */
void rknn_ctx_close(rknn_ctx_t* c) {
    if (!c) return;
    if (c->ctx) rknn_destroy(c->ctx);  /* 销毁 NPU 运行时上下文 */
    free(c->model_data);                /* 释放模型文件数据        */
    free(c->input_attrs);               /* 释放输入属性数组        */
    free(c->output_attrs);              /* 释放输出属性数组        */
    free(c);
}

/*
 * 设置模型输入
 * 将预处理后的图像数据传递给 NPU
 *
 * @inputs：rknn_input 数组（描述输入数据的位置/格式/大小）
 * @n：输入张量数量（YOLOv5s 为 1）
 */
int rknn_ctx_set_inputs(rknn_ctx_t* c, const rknn_input* inputs, uint32_t n) {
    return c && rknn_inputs_set(c->ctx, n, (rknn_input*)inputs) >= 0;
}

/*
 * 执行 NPU 推理
 * 这个调用将计算任务提交给 NPU 硬件，是推理的核心步骤
 * NPU 推理期间 CPU 可继续执行其他代码，实现硬件并行
 * 单帧推理耗时约 25ms（INT8 量化，640×640 输入）
 */
int rknn_ctx_run(rknn_ctx_t* c) {
    return c && rknn_run(c->ctx, NULL) >= 0;
}

/*
 * 获取模型输出
 * 从 NPU 的输出缓冲区读取推理结果
 *
 * YOLOv5s 三个输出的内容：
 *   [0] 1×255×80×80  INT8 张量（小目标检测头）
 *   [1] 1×255×40×40  INT8 张量（中目标检测头）
 *   [2] 1×255×20×20  INT8 张量（大目标检测头）
 *
 * @outputs：预先分配的输出描述数组
 * @n：输出数量
 */
int rknn_ctx_get_outputs(rknn_ctx_t* c, rknn_output* outputs, uint32_t n) {
    return c && rknn_outputs_get(c->ctx, n, outputs, NULL) >= 0;
}

/*
 * 释放输出缓冲区
 * 每次 rknn_outputs_get() 后必须调用此函数释放 NPU 内部的输出缓冲区
 * 否则会导致内存泄漏，甚至耗尽 NPU 的 DMA 内存
 */
int rknn_ctx_release_outputs(rknn_ctx_t* c, rknn_output* outputs, uint32_t n) {
    return c && rknn_outputs_release(c->ctx, n, outputs) >= 0;
}

/* 以下为属性访问器（Getters） */
uint32_t rknn_ctx_input_w(rknn_ctx_t* c)  { return c ? c->in_w  : 640; }
uint32_t rknn_ctx_input_h(rknn_ctx_t* c)  { return c ? c->in_h  : 640; }
uint32_t rknn_ctx_input_c(rknn_ctx_t* c)  { return c ? c->in_c  : 3; }
uint32_t rknn_ctx_output_count(rknn_ctx_t* c) { return c ? c->out_n : 3; }
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* c, uint32_t idx) {
    return (c && c->output_attrs && idx < c->out_n) ? &c->output_attrs[idx] : NULL;
}

#endif
