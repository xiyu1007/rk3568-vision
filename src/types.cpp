// ============================================================================
// types.cpp — 基础类型实现
// ============================================================================

#include "types.hpp"

#include <ctime>

namespace vision {

// ---------------------------------------------------------------------------
// 读取当前单调时钟（微秒）
// ---------------------------------------------------------------------------
// CLOCK_MONOTONIC：从系统启动开始计时的单调时钟，不受墙钟调整影响。
// 换算：秒 * 1e6 + 纳秒 / 1e3 = 微秒。
TimestampUs nowUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

// ---------------------------------------------------------------------------
// 分配一帧并预留 NV12 数据缓冲区
// ---------------------------------------------------------------------------
FramePtr makeFrame(uint32_t width, uint32_t height, uint32_t stride) {
    auto f = std::make_shared<Frame>();
    f->width  = width;
    f->height = height;
    // stride 为 0 时按宽度对齐（通常 V4L2 的 stride 就是 width，除非有对齐要求）。
    f->stride = (stride == 0) ? width : stride;
    // NV12 总大小 = Y 平面(width*height) + UV 平面(width*height/2)。
    f->nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
    return f;
}

} // namespace vision
