// ============================================================================
// mock_inferencer.cpp — 假推理器实现
// ============================================================================

#include "mock_inferencer.hpp"

#include <cstring>

namespace vision {

bool MockInferencer::detect(const FramePtr& frame) {
    if (!frame) return false;

    // 用帧序号产生缓慢移动的相位（0~2π），让框在画面里游走，便于目视验证。
    const float phase = static_cast<float>(frame->seq % 100) / 100.0f * 6.283185f;
    const int   w     = static_cast<int>(frame->width);
    const int   h     = static_cast<int>(frame->height);

    DetectResult& det = frame->detect;
    det.count = 0;

    // 框 1："person"，位置沿水平方向往返移动。
    DetectBox& b0 = det.boxes[det.count++];
    b0.x = static_cast<int>(w * 0.10f + w * 0.40f * (0.5f + 0.5f * std::sin(phase)));
    b0.y = static_cast<int>(h * 0.15f);
    b0.w = w / 6;
    b0.h = h / 3;
    b0.class_id = 0;                       // COCO 0 = person
    b0.conf = 0.92f;
    std::strncpy(b0.label, "person", sizeof(b0.label) - 1);

    // 框 2："car"，位置沿垂直方向往返移动。
    DetectBox& b1 = det.boxes[det.count++];
    b1.x = static_cast<int>(w * 0.55f);
    b1.y = static_cast<int>(h * 0.30f + h * 0.35f * (0.5f + 0.5f * std::cos(phase)));
    b1.w = w / 4;
    b1.h = h / 4;
    b1.class_id = 2;                       // COCO 2 = car
    b1.conf = 0.87f;
    std::strncpy(b1.label, "car", sizeof(b1.label) - 1);

    frame->inference_ts = nowUs();
    return true;
}

} // namespace vision
