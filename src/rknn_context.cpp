/*
 * ==========================================================================
 * rknn_context.cpp — RKNN 模型上下文的 C++ RAII 封装
 * ==========================================================================
 *
 * **设计目的**：提供资源安全（RAII）的 RKNN 模型管理
 *
 * **为什么不直接用 rknn.c？**
 *   rknn.c 是纯 C 封装，需要手动管理内存（malloc/free + rknn_destroy）
 *   本 C++ 封装使用 RAII（Resource Acquisition Is Initialization）模式：
 *     - 构造时加载模型 → init() 创建上下文
 *     - 析构时自动释放全部资源（rknn_destroy + free model_data）
 *     - std::unique_ptr 管理属性数组内存
 *     - 禁止拷贝（delete 拷贝构造/赋值），防止双重释放
 *
 * **RknnContext 生命周期**：
 *   1. RknnContext() — 默认构造
 *   2. init(model_path, npu_core) — 加载模型、创建上下文、查询属性
 *   3. [推理循环] set_inputs() → run() → get_outputs() → release_outputs()
 *   4. ~RknnContext() — 析构，自动清理所有资源
 *
 * **与 Detector 的关系**：
 *   Detector 持有 std::unique_ptr<RknnContext> rknn_
 *   Detector 负责预处理/后处理（图像变换、解码框、NMS）
 *   RknnContext 负责纯粹的模型加载和 NPU 调用
 */

#include "rknn_context.hpp"

extern "C" {
#include "logger.h"
}

#ifdef X86_DEBUG

/* ==========================================================================
 *  x86 调试桩 — 所有方法返回 false
 *  在开发机上编译通过但不执行实际推理
 * ========================================================================== */
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

/* ==========================================================================
 *  真实 RKNN 实现 — 运行在 RK3568 ARM64 上
 * ========================================================================== */

#include <cstdio>

namespace rk3568_vision {

/*
 * 将模型文件完整加载到 std::vector<uint8_t> 中
 * 使用 RAII 管理内存：函数返回 vector，调用者自动持有
 * 如果加载失败返回空 vector，init() 中检查后返回 false
 */
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

/*
 * 析构函数：自动清理 RKNN 上下文
 * 如果 ctx_ != 0，销毁运行时，释放 NPU 资源
 */
RknnContext::~RknnContext() {
    if (ctx_ != 0) { rknn_destroy(ctx_); ctx_ = 0; }
}

/*
 * 初始化 RKNN 上下文
 *
 * 完整流程：
 *   1. 加载 .rknn 模型文件到 model_data_（std::vector<uint8_t>）
 *   2. rknn_init() 创建 NPU 运行时
 *   3. rknn_set_core_mask() 指定 NPU 核心
 *   4. rknn_query() 获取输入/输出数量
 *   5. rknn_query() 获取每个输入/输出张量的属性
 *   6. 解析模型输入尺寸（input_width_, input_height_, input_channel_）
 *
 * 输出张量属性日志示例：
 *   Output[0]: dims=[1,255,80,80] nd=4 size=1632000 fmt=0 type=2 scale=0.0039 zp=0
 *   Output[1]: dims=[1,255,40,40] nd=4 size=408000 fmt=0 type=2 scale=0.0041 zp=0
 *   Output[2]: dims=[1,255,20,20] nd=4 size=102000 fmt=0 type=2 scale=0.0044 zp=0
 *
 *   其中：
 *     fmt=0 → NCHW；type=2 → INT8
 *     scale 和 zp（zero point）用于 INT8→FP32 反量化
 *     反量化公式：float_val = (int8_val - zp) * scale
 */
bool RknnContext::init(const std::string& model_path, uint32_t npu_core) {
    /* 1. 加载模型文件 */
    model_data_ = load_file(model_path);
    if (model_data_.empty()) return false;

    /* 2. 创建 RKNN 运行时 */
    int ret = rknn_init(&ctx_, model_data_.data(), model_data_.size(), 0, nullptr);
    if (ret < 0) { LOG_ERROR("rknn_init failed: ret=%d", ret); return false; }

    /* 3. 设置 NPU 核心 */
    rknn_set_core_mask(ctx_, (npu_core == 0) ? RKNN_NPU_CORE_0
                          : (npu_core == 1) ? RKNN_NPU_CORE_1 : RKNN_NPU_CORE_2);

    /* 4. 查询输入/输出数量 */
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0) { LOG_ERROR("rknn_query IO failed"); return false; }
    output_count_ = io_num_.n_output;

    LOG_INFO("RKNN model IO: %u inputs, %u outputs", io_num_.n_input, io_num_.n_output);

    /*
     * 5. 查询输入张量属性
     * 使用 std::unique_ptr + 自定义 AttrDeleter 管理内存
     * calloc 分配（初始化为零），free 释放
     */
    input_attrs_.reset(static_cast<rknn_tensor_attr*>(
        calloc(io_num_.n_input, sizeof(rknn_tensor_attr))));
    for (uint32_t i = 0; i < io_num_.n_input; ++i) {
        input_attrs_.get()[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR,
                          &input_attrs_.get()[i], sizeof(rknn_tensor_attr));
        if (ret < 0) { LOG_ERROR("input attr[%u] failed", i); return false; }
    }

    /*
     * 6. 查询输出张量属性
     * 打印详细输出信息，方便调试（形状、数据类型、量化参数）
     */
    output_attrs_.reset(static_cast<rknn_tensor_attr*>(
        calloc(io_num_.n_output, sizeof(rknn_tensor_attr))));
    for (uint32_t i = 0; i < io_num_.n_output; ++i) {
        output_attrs_.get()[i].index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR,
                   &output_attrs_.get()[i], sizeof(rknn_tensor_attr));
        auto& oa = output_attrs_.get()[i];
        int sz = 1;
        for (uint32_t d = 0; d < oa.n_dims; d++) sz *= oa.dims[d];
        LOG_INFO("Output[%u]: dims=[%d,%d,%d,%d] nd=%d size=%d fmt=%d type=%d scale=%.4f zp=%d",
                 i, oa.dims[0],oa.dims[1],oa.dims[2],oa.dims[3],
                 oa.n_dims, sz, oa.fmt, oa.type, oa.scale, (int)oa.zp);
    }

    /*
     * 7. 解析输入尺寸
     * NCHW 格式：dims[0]=batch, dims[1]=channels, dims[2]=height, dims[3]=width
     * NHWC 格式：dims[0]=batch, dims[1]=height, dims[2]=width, dims[3]=channels
     */
    auto& attr = input_attrs_.get()[0];
    if (attr.fmt == RKNN_TENSOR_NCHW) {
        input_channel_ = attr.dims[1]; input_height_ = attr.dims[2]; input_width_ = attr.dims[3];
    } else {
        input_height_ = attr.dims[1]; input_width_ = attr.dims[2]; input_channel_ = attr.dims[3];
    }
    LOG_INFO("Model input: %ux%ux%u type=%d fmt=%s scale=%.4f zp=%d fl=%d qnt=%d",
             input_width_, input_height_, input_channel_,
             (int)attr.type,
             attr.fmt == RKNN_TENSOR_NCHW ? "NCHW" : "NHWC",
             attr.scale, (int)attr.zp, (int)attr.fl, (int)attr.qnt_type);
    return true;
}

/*
 * 设置 NPU 输入数据
 * 将预处理后的图像（640×640×3 RGB，UINT8）传入 NPU
 */
bool RknnContext::set_inputs(const rknn_input* inputs, uint32_t n_input) {
    int ret = rknn_inputs_set(ctx_, n_input, const_cast<rknn_input*>(inputs));
    if (ret < 0) LOG_ERROR("rknn_inputs_set failed: %d", ret);
    return ret >= 0;
}

/*
 * 执行 NPU 推理
 * 提交计算任务到 NPU 硬件，CPU 在此时可以继续处理其他任务
 *
 * 性能：单帧 YOLOv5s INT8 推理约 25ms（640×640 输入）
 * NPU 功耗：约 1-2W（远低于 CPU 做同样推理的 4-5W）
 */
bool RknnContext::run() {
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) LOG_ERROR("rknn_run failed: %d", ret);
    return ret >= 0;
}

/*
 * 获取 NPU 输出
 * 输出为 INT8 量化张量，需要后续反量化和后处理
 */
bool RknnContext::get_outputs(rknn_output* outputs, uint32_t n_output) {
    int ret = rknn_outputs_get(ctx_, n_output, outputs, nullptr);
    if (ret < 0) LOG_ERROR("rknn_outputs_get failed: %d", ret);
    return ret >= 0;
}

/*
 * 释放输出缓冲区
 * 每次 get_outputs() 后必须调用此函数
 * 释放 NPU 侧的输出内存，避免内存泄漏
 */
bool RknnContext::release_outputs(rknn_output* outputs, uint32_t n_output) {
    return rknn_outputs_release(ctx_, n_output, outputs) >= 0;
}

} // namespace rk3568_vision

#endif
