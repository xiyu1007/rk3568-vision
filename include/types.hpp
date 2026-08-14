// ============================================================================
// types.hpp — 全项目基础类型定义
// ============================================================================
//
// 本文件定义了流水线中贯穿各模块的核心数据结构：
//   - TimestampUs   : 时间戳（微秒，CLOCK_MONOTONIC 单调时钟）
//   - DetectBox     : 单个检测框
//   - DetectResult  : 一帧的检测结果集合
//   - Frame         : 流水线中传递的视频帧（NV12 数据 + 元数据 + 检测结果）
//
// 为什么用 CLOCK_MONOTONIC（单调时钟）？
//   - 它不受系统时间调整（NTP 校时、手动改时间）影响
//   - 测量两段时间差时永远为正，适合计算端到端延迟
// ============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace vision {

// ---------------------------------------------------------------------------
// 时间戳
// ---------------------------------------------------------------------------
// 微秒精度足以覆盖毫秒级的性能统计需求。
using TimestampUs = uint64_t;

// 读取当前单调时钟时间（微秒）。
// 实现放在头文件里用 inline，避免单独建一个源文件。
TimestampUs nowUs();

// ---------------------------------------------------------------------------
// 检测结果相关常量与结构
// ---------------------------------------------------------------------------
constexpr int kMaxBoxes     = 64;   // 单帧最多输出的检测框数量
constexpr int kMaxLabelLen  = 64;   // 类别标签字符串最大长度（如 "person"）

// 单个检测框。
// 坐标 x/y/w/h 均基于【原始采集图像】的像素坐标系（不是模型 640×640 坐标系）。
struct DetectBox {
    int    x = 0;                   // 左上角 x 坐标（像素）
    int    y = 0;                   // 左上角 y 坐标（像素）
    int    w = 0;                   // 框宽度（像素）
    int    h = 0;                   // 框高度（像素）
    int    class_id = -1;           // 类别编号（对应 COCO 80 类，0=person）
    float  conf = 0.0f;             // 置信度（0.0 ~ 1.0）
    char   label[kMaxLabelLen] = {0}; // 类别名称字符串（如 "person"）
};

// 一帧的完整检测结果。
struct DetectResult {
    uint32_t  count = 0;            // 有效检测框数量（0 ~ kMaxBoxes）
    DetectBox boxes[kMaxBoxes];     // 检测框数组
};

// ---------------------------------------------------------------------------
// Frame — 流水线核心数据单元
// ---------------------------------------------------------------------------
//
// 一帧视频数据（NV12 格式）及其元数据，从采集线程一路流转到编码线程。
// 使用 std::shared_ptr<Frame> 在模块间传递指针，实现零数据拷贝。
//
// NV12 内存布局（总大小 = width * height * 1.5 字节）：
//   [ Y 平面  ]  width × height 字节 —— 亮度，每像素 1 字节
//   [ UV 平面 ]  (width × height)/2 字节 —— 色度，UV 交错，每 4 个 Y 共享一组 UV
struct Frame {
    uint64_t   seq = 0;             // 全局帧序号（单调递增，用于追踪）
    uint32_t   width = 0;           // 图像宽度（像素）
    uint32_t   height = 0;          // 图像高度（像素）
    uint32_t   stride = 0;          // Y 平面行步长（字节，可能 > width，V4L2 会对齐）

    TimestampUs capture_ts = 0;     // 采集完成时间戳（微秒）
    TimestampUs inference_ts = 0;   // 推理完成时间戳（微秒）

    std::vector<uint8_t> nv12;      // NV12 原始数据（本对象独占拥有）
    DetectResult         detect;     // 该帧的检测结果（推理线程填充）
};

// 帧的共享指针别名，流水线各模块统一使用它传递帧。
using FramePtr = std::shared_ptr<Frame>;

// ---------------------------------------------------------------------------
// 工厂函数
// ---------------------------------------------------------------------------

// 分配一帧，并为其 NV12 数据缓冲区预留 width*height*1.5 字节。
// stride 传入 Y 平面行步长；若为 0 则默认为 width。
FramePtr makeFrame(uint32_t width, uint32_t height, uint32_t stride);

// 深拷贝一帧（复制 NV12 数据与元数据，但不复制检测结果）。
// 稳帧器用它维护一份私有副本，避免与下游线程共享同一块 NV12 缓冲导致数据竞争。
inline FramePtr cloneFrame(const FramePtr& src) {
    auto f = std::make_shared<Frame>();
    f->seq         = src->seq;
    f->width       = src->width;
    f->height      = src->height;
    f->stride      = src->stride;
    f->capture_ts  = src->capture_ts;
    f->inference_ts = src->inference_ts;
    f->nv12        = src->nv12;   // std::vector 赋值 = 深拷贝数据
    // detect 保持默认（空），由推理线程对每帧重新填充
    return f;
}

} // namespace vision
