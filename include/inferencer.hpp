// ============================================================================
// inferencer.hpp — 推理器接口（策略模式）
// ============================================================================
//
// 职责：抽象“输入一帧视频 → 填充该帧的检测结果”。
//
// 采用策略模式，把「推理」从流水线中解耦，便于在不同平台切换实现：
//   - RknnInferencer : aarch64 上真实调用 RKNN NPU 推理（YOLOv5s INT8）
//   - MockInferencer : x86 开发机上的假推理（生成固定检测框，供链路联调）
//
// 流水线只依赖本接口，不关心具体是哪个实现。
// ============================================================================

#pragma once

#include <memory>

#include "config.hpp"
#include "types.hpp"

namespace vision {

class Inferencer {
public:
    virtual ~Inferencer() = default;

    // 初始化：加载模型/标签等资源。成功返回 true。
    virtual bool init() = 0;

    // 对一帧执行检测，结果写回 frame->detect。成功返回 true。
    virtual bool detect(const FramePtr& frame) = 0;

    // 实现名（日志用）。
    virtual const char* name() const = 0;
};

// 工厂函数：根据配置与编译平台选择实现。
//   - aarch64 + backend != "mock" → RknnInferencer（真实 NPU）
//   - 其余情况                    → MockInferencer（假推理）
std::unique_ptr<Inferencer> createInferencer(const Config& cfg);

} // namespace vision
