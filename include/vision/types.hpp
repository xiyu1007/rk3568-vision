// ============================================================================
// types.hpp — 核心类型定义
// ============================================================================
//
// 全项目共用的核心数据结构集中在此：
//   - 时间戳（单调时钟微秒）
//   - 检测结果（DetectBox / DetectResult）
//   - 视频帧（Frame，支持 V4L2 dmabuf 零拷贝 / mp4 解码两种数据来源）
//   - 编码包（AVPacket 共享指针）
//
// 设计要点：
//   Frame 不直接持有裸内存，而是通过 dma_fd（V4L2 零拷贝）或 nv12_cpu（CPU 内存）
//   两种方式之一引用 NV12 数据。dmabuf 帧的生命周期由 CameraSource 通过 shared_ptr
//   自定义 deleter 管理（引用计数归零时归还 V4L2 buffer）。
// ============================================================================

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>   // AVPacket（编码包类型）
}

namespace vision {

// ---------------------------------------------------------------------------
// 时间戳
// ---------------------------------------------------------------------------

// 单调时钟微秒时间戳（用于测延迟，不受系统墙钟调整影响）。
using TimestampUs = uint64_t;

// 获取当前单调时钟的微秒时间戳。
inline TimestampUs GetCurrentTimestampUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

// ---------------------------------------------------------------------------
// 检测结果
// ---------------------------------------------------------------------------

// 单帧最大检测框数量。
constexpr int kMaxBoxes = 64;

// 类别标签最大长度（含结尾 '\0'）。
constexpr int kMaxLabelLength = 64;

// 单个检测框，坐标为原图像素坐标系。
struct DetectBox {
    int x = 0;              // 框左上角 X
    int y = 0;              // 框左上角 Y
    int width = 0;          // 框宽
    int height = 0;         // 框高
    int class_id = -1;      // 类别 ID
    float confidence = 0.0f;  // 置信度（0~1）
    char label[kMaxLabelLength] = {0};  // 类别标签文本
};

// 一帧的检测结果。
struct DetectResult {
    uint32_t count = 0;               // 检测框数量
    DetectBox boxes[kMaxBoxes];       // 检测框数组
};

// ---------------------------------------------------------------------------
// 视频帧（流水线核心数据单元）
// ---------------------------------------------------------------------------

// 一帧 NV12 视频数据，附带检测结果与元信息。
//
// NV12 数据通过 nv12_data（虚拟地址）访问，来源二选一：
//   1) V4L2 摄像头：零拷贝，nv12_data 指向 DMA buffer 的 mmap 映射地址，
//      数据不经 CPU 拷贝；buffer 归还由 Frame 的 shared_ptr 自定义 deleter
//      负责（引用计数归零时 QBUF）。
//   2) mp4 文件：FFmpeg 解码到 CPU 内存，nv12_data 指向该内存（由 nv12_cpu 持有）。
struct Frame {
    uint64_t sequence = 0;              // 帧序号（单调递增，用于追踪生命周期）
    uint32_t width = 0;                 // 帧宽
    uint32_t height = 0;                // 帧高
    TimestampUs capture_timestamp = 0;    // 采集完成时间戳
    TimestampUs inference_timestamp = 0;  // 推理完成时间戳

    // ---- NV12 数据（虚拟地址）----
    const uint8_t* nv12_data = nullptr;   // NV12 数据起始地址（Y 平面）
    uint32_t nv12_stride = 0;             // NV12 行跨度（V4L2 的 bytesperline，mp4 时=width）

    // ---- V4L2 dmabuf 元信息（mp4 输入时为空）----
    std::vector<int> dma_fds;             // 各 plane 的 dmabuf fd（EXPBUF 导出，供 RGA fd 输入）
    uint32_t buffer_index = 0;            // V4L2 buffer index（归还 QBUF 用）

    // ---- CPU 内存持有者（mp4 输入时有效）----
    std::shared_ptr<std::vector<uint8_t>> nv12_cpu;

    DetectResult detection;             // 检测结果
};

// 帧共享指针（shared_ptr 自定义 deleter 负责 dmabuf buffer 归还）。
using FramePtr = std::shared_ptr<Frame>;

// ---------------------------------------------------------------------------
// 编码包（AVPacket 共享指针）
// ---------------------------------------------------------------------------

// AVPacket 的自定义 deleter，释放 FFmpeg 资源。
struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
    }
};

// 编码包共享指针。
using PacketPtr = std::shared_ptr<AVPacket>;

// 创建一个空的编码包。
inline PacketPtr CreatePacket() {
    return PacketPtr(av_packet_alloc(), PacketDeleter{});
}

} // namespace vision
