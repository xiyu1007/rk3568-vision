// ============================================================================
// pipeline.hpp — 流水线编排
// ============================================================================
//
// 把 采集 → 稳帧 → 推理 → 编码 → 推流/录制 组织成多线程生产者-消费者流水线，
// 并内置稳帧器、系统监控、性能统计。
//
// 线程数据流：
//   采集线程 ─cap_q─▶ 稳帧线程 ─inf_q─▶ 推理线程 ─enc_q─▶ 编码线程
//                                                        ├─push_q─▶ 推流线程(RTMP)
//                                                        └─record_q▶ 录制线程(MP4)
//   监控线程（周期打印 CPU/内存/温度 + 各阶段延迟）
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "capture.hpp"
#include "common.hpp"
#include "encoder.hpp"
#include "inferencer.hpp"
#include "ring_buffer.hpp"

namespace vision {

// 各阶段性能统计（原子，多线程安全）。
struct Perf {
    std::atomic<int64_t> capture_us{0};      // 采集延迟
    std::atomic<int64_t> inference_us{0};    // 推理延迟
    std::atomic<int64_t> encode_us{0};       // 编码延迟
    std::atomic<int64_t> total_frames{0};    // 累计编码帧数
    std::atomic<int64_t> dropped_frames{0};  // 累计丢帧数
    std::atomic<int64_t> pushed_frames{0};   // 累计推流包数
    std::atomic<int64_t> recorded_frames{0}; // 累计录制包数
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool init();   // 打开采集/推理/编码/封装
    bool start();  // 启动采集流 + 创建所有线程
    void stop();   // 优雅停止 + 释放资源
    bool running() const { return running_.load(); }

private:
    // ---- 工作线程 ----
    void captureLoop();    // 采集 → cap_q
    void pacerLoop();      // 稳帧：cap_q → inf_q（节拍输出）
    void inferenceLoop();  // inf_q → 推理 → enc_q
    void encodeLoop();     // enc_q → 画框+编码 → push_q/record_q
    void pushLoop();       // push_q → RTMP
    void recordLoop();     // record_q → MP4
    void monitorLoop();    // 周期打印系统状态

    void dispatchPacket(const PacketPtr& pkt);
    bool openRtmp();
    bool openMp4();

    Config cfg_;
    Capture                        capture_;
    std::unique_ptr<Inferencer>    inferencer_;
    H264Encoder                    encoder_;
    Muxer                          rtmp_muxer_, mp4_muxer_;

    // 帧/包队列（有界环形缓冲 + 条件变量）
    RingBuffer<FramePtr>  cap_q_{8};
    RingBuffer<FramePtr>  inf_q_{8};
    RingBuffer<FramePtr>  enc_q_{8};
    RingBuffer<PacketPtr> push_q_{16};
    RingBuffer<PacketPtr> record_q_{64};

    std::thread capture_t_, pacer_t_, inference_t_, encode_t_;
    std::thread push_t_, record_t_, monitor_t_;

    std::atomic<bool> running_{false};
    uint64_t seq_ = 0;
    std::vector<uint8_t> extradata_;   // SPS/PPS 副本（重连用）
    bool extradata_ready_ = false;     // 首帧后 extradata 是否已就绪
    uint32_t width_ = 0, height_ = 0, fps_ = 0;

    Perf perf_;

    // 监控用 CPU 快照
    uint64_t last_cpu_total_ = 0, last_cpu_idle_ = 0;
};

} // namespace vision
