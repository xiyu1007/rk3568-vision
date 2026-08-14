// ============================================================================
// mock_inferencer.hpp — 假推理器（x86 联调用）
// ============================================================================
//
// 职责：在没有 RKNN NPU 的 x86 开发机上，生成若干“假”检测框，让
//       采集 → 稳帧 → 推理 → 编码 → 推流/录制 整条流水线能端到端跑通。
//
// 生成策略：
//   用帧序号 seq 计算两个框的位置（随时间缓慢移动），类别固定为
//   "person" 和 "car"，用来验证画框、编码、推流、录制各环节是否正常。
// ============================================================================

#pragma once

#include <cmath>

#include "inferencer.hpp"

namespace vision {

class MockInferencer : public Inferencer {
public:
    explicit MockInferencer(const Config& cfg)
        : conf_(cfg.inference.conf_threshold) {}

    // 假推理无需加载任何资源。
    bool init() override { return true; }

    bool detect(const FramePtr& frame) override;

    const char* name() const override { return "mock"; }

private:
    float conf_;   // 占位保留阈值语义（假框固定置信度，仅用于展示）
};

} // namespace vision
