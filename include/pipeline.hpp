// ============================================================================
// pipeline.hpp — 流水线编排（核心调度）
// ============================================================================
//
// 职责：把采集、稳帧、推理、编码、推流、录制六个环节串联起来，
//       用生产者-消费者模型组织成多线程流水线。
//
// 线程与数据流：
//
//   采集线程 ──cap_q──▶ 稳帧线程(FramePacer) ──inf_q──▶ 推理线程
//                                                            │
//                                                       enc_q│(NV12+检测框)
//                                                            ▼
//                                                        编码线程
//                                                     ┌────┴─────┐
//                                                push_q│     record_q│
//                                                      ▼          ▼
//                                                   推流线程    录制线程
//                                                  (RTMP)      (MP4)
//
// 生命周期：
//   init()  打开采集/推理器/编码器/封装器（任一失败则降级或返回失败）
//   start() 启动采集流 + 创建所有工作线程
//   stop()  置停止标志 → 关闭队列唤醒线程 → join → 释放资源
// ============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "config.hpp"
#include "encoder.hpp"
#include "encoded_packet.hpp"
#include "frame_pacer.hpp"
#include "inferencer.hpp"
#include "muxer.hpp"
#include "ring_buffer.hpp"
#include "types.hpp"
#include "v4l2_capture.hpp"

namespace vision {

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // 初始化所有模块（采集/推理/编码/封装）。
    bool init();

    // 启动流水线（STREAMON + 创建工作线程）。
    bool start();

    // 停止流水线（优雅退出，释放全部资源）。
    void stop();

    bool running() const { return running_.load(); }

private:
    // ---- 各工作线程主循环 ----
    void captureLoop();    // 采集 → cap_q
    void inferenceLoop();  // inf_q → 推理 → enc_q
    void encodeLoop();     // enc_q → 画框+编码 → push_q / record_q
    void pushLoop();       // push_q → RTMP 推流（含重连）
    void recordLoop();     // record_q → MP4 录制

    // 把编码包分发给推流/录制队列。
    void dispatchPacket(const PacketPtr& pkt);
    // 打开 RTMP 推流器（供初始化和断线重连复用）。
    bool openRtmp();
    // 打开 MP4 录制器。
    bool openMp4();

    Config cfg_;

    // ---- 采集 / 推理 / 编码 / 封装 ----
    V4l2Capture                    capture_;
    std::unique_ptr<Inferencer>    inferencer_;
    H264Encoder                    encoder_;
    Muxer                          rtmp_muxer_;
    Muxer                          mp4_muxer_;

    // ---- 帧队列（有界环形缓冲 + 条件变量）----
    RingBuffer<FramePtr> cap_q_;     // 采集 → 稳帧
    RingBuffer<FramePtr> inf_q_;     // 稳帧 → 推理
    RingBuffer<FramePtr> enc_q_;     // 推理 → 编码

    // ---- 编码包队列 ----
    RingBuffer<PacketPtr> push_q_;   // 编码 → 推流
    RingBuffer<PacketPtr> record_q_; // 编码 → 录制

    FramePacer pacer_;               // 稳帧器（自带线程）

    // ---- 工作线程 ----
    std::thread capture_thread_;
    std::thread inference_thread_;
    std::thread encode_thread_;
    std::thread push_thread_;
    std::thread record_thread_;

    std::atomic<bool> running_{false};   // 运行标志
    uint64_t seq_ = 0;                   // 全局帧序号

    // 编码器 extradata（SPS/PPS）副本，供重连时重建封装器。
    std::vector<uint8_t> extradata_;

    uint32_t width_  = 0;   // 采集宽度（缓存）
    uint32_t height_ = 0;   // 采集高度
    uint32_t fps_    = 0;   // 采集帧率
};

} // namespace vision
