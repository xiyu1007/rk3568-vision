// ============================================================================
// pipeline.hpp — 流水线协调器
// ============================================================================
//
// 组合各模块（采集/稳帧/推理/编码/推流/录制），组织成多线程生产者-消费者流水线。
// 借鉴参考项目 around_view_app 的 CAVMSystemModule：协调器只做协调，不承载具体逻辑，
// 模块间通过回调解耦 + 有界队列通信。
//
// 线程数据流：
//   采集线程（CameraSource 内部）─回调(只入队)→ capture_queue_ ─稳帧→ inference_queue_
//     ─推理→ encode_queue_ ─编码→ push_queue_（推流）/ record_queue_（录制）
//   监控线程：周期打印 CPU/内存/温度 + 各阶段延迟。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "vision/camera_source.hpp"
#include "vision/config.hpp"
#include "vision/h264_encoder.hpp"
#include "vision/inferencer.hpp"
#include "vision/mp4_recorder.hpp"
#include "vision/mpp_encoder.hpp"
#include "vision/ring_buffer.hpp"
#include "vision/rtmp_streamer.hpp"
#include "vision/types.hpp"

namespace vision {

// 各阶段性能统计（原子，多线程安全）。
struct PerfStats {
    std::atomic<int64_t> capture_us{0};      // 采集延迟（微秒）
    std::atomic<int64_t> inference_us{0};    // 推理延迟（微秒）
    std::atomic<int64_t> encode_us{0};       // 编码延迟（微秒）
    std::atomic<int64_t> total_frames{0};    // 累计编码帧数
    std::atomic<int64_t> dropped_frames{0};  // 累计丢帧数
    std::atomic<int64_t> pushed_frames{0};   // 累计推流包数
    std::atomic<int64_t> recorded_frames{0}; // 累计录制包数
};

class Pipeline {
public:
    explicit Pipeline(const Config& config);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // 打开采集/推理/编码（不启动线程）。
    bool Initialize();

    // 启动采集流 + 创建所有工作线程。
    bool Start();

    // 优雅停止 + 释放资源（幂等）。
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    // ---- 采集回调（回调解耦：只入队，不做重活）----
    void OnFrameCaptured(FramePtr frame);

    // ---- 工作线程 ----
    void PacerLoop();      // 稳帧：capture_queue_ → inference_queue_
    void InferenceLoop();  // 推理：inference_queue_ → encode_queue_
    void EncodeLoop();     // 编码：encode_queue_ → push_queue_/record_queue_
    void PushLoop();       // 推流：push_queue_ → RTMP
    void RecordLoop();     // 录制：record_queue_ → MP4
    void MonitorLoop();    // 监控：周期打印系统状态

    // ---- 编码包分发 ----
    void DispatchPacket(const PacketPtr& packet);

    Config config_;
    CameraSource camera_source_;
    std::unique_ptr<Inferencer> inferencer_;
    H264Encoder encoder_;                          // 软编（libx264）
    std::unique_ptr<MppEncoder> mpp_encoder_;      // 硬编（Rockchip MPP）
    bool use_hardware_ = false;                    // 是否用 MPP 硬编
    std::unique_ptr<RtmpStreamer> rtmp_streamer_;
    std::unique_ptr<Mp4Recorder> mp4_recorder_;

    // 帧/包队列（有界环形缓冲 + 条件变量）。
    RingBuffer<FramePtr> capture_queue_{8};
    RingBuffer<FramePtr> inference_queue_{8};
    RingBuffer<FramePtr> encode_queue_{8};
    RingBuffer<PacketPtr> push_queue_{16};
    RingBuffer<PacketPtr> record_queue_{64};

    std::thread pacer_thread_;
    std::thread inference_thread_;
    std::thread encode_thread_;
    std::thread push_thread_;
    std::thread record_thread_;
    std::thread monitor_thread_;

    std::atomic<bool> running_{false};
    uint64_t frame_sequence_ = 0;      // 帧序号（回调里递增）
    std::vector<uint8_t> extradata_;   // SPS/PPS 副本（重连用）
    bool extradata_ready_ = false;     // 首帧后 extradata 是否就绪
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 0;

    PerfStats perf_stats_;

    // 监控用 CPU 快照。
    uint64_t last_cpu_total_ = 0;
    uint64_t last_cpu_idle_ = 0;
};

} // namespace vision
