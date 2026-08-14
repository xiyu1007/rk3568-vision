// ============================================================================
// frame_pacer.hpp — 稳帧器（帧率稳定节拍器）
// ============================================================================
//
// 职责：位于【采集之后、推理之前】，把采集线程送来的、帧间隔抖动的帧，
//      按目标帧率（如 25fps）稳定地节拍输出，保证下游推理/编码拿到
//      均匀节奏的帧流。
//
// 为什么需要稳帧？
//   摄像头采集受曝光/ISP/总线调度影响，实际帧间隔会有 ±几毫秒抖动；
//   若不做节拍，抖动会一路传导到 RTMP 输出，导致画面卡顿、码率不稳。
//
// 核心策略：
//   1. 【节拍】以单调时钟为基准，每个周期(1/fps)输出一帧，输出间隔恒定。
//   2. 【补帧】某周期没有新帧到达时，复制上一帧输出（allow_duplicate=true），
//      维持恒定的输出帧率。
//   3. 【丢帧】上游采集快于目标帧率时，队列有界 + 丢最旧，自然丢弃多余帧。
//   4. 【重新对齐】若因某种原因落后一个周期以上，直接对齐到当前时间，
//      避免追帧式连发（那会造成延迟雪崩）。
//
// 线程安全：
//   稳帧器维护一份【私有副本】last_，与下游共享的帧不是同一个对象，
//   因此下游（推理写 detect、编码写 NV12 画框）不会与稳帧器产生数据竞争。
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "config.hpp"
#include "ring_buffer.hpp"
#include "types.hpp"

namespace vision {

class FramePacer {
public:
    FramePacer() = default;
    ~FramePacer();

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // 启动稳帧线程。
    //   in   : 输入队列（来自采集线程）
    //   out  : 输出队列（送往推理线程）
    //   cfg  : 稳帧配置（目标帧率、是否补帧等）
    void start(RingBuffer<FramePtr>* in, RingBuffer<FramePtr>* out,
               const PacerConfig& cfg);

    // 停止稳帧线程（唤醒并 join）。
    void stop();

private:
    // 线程主循环。
    void run();

    RingBuffer<FramePtr>* in_  = nullptr;   // 输入（采集 → 稳帧）
    RingBuffer<FramePtr>* out_ = nullptr;   // 输出（稳帧 → 推理）

    PacerConfig cfg_;                       // 配置副本
    std::thread thread_;                    // 稳帧线程
    std::atomic<bool> running_{false};      // 运行标志
};

} // namespace vision
