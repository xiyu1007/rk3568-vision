// ============================================================================
// perf.hpp — 全局性能统计（原子变量，多线程安全）
// ============================================================================
//
// 各工作线程在关键路径写入本结构，监控线程周期读取并输出，用于
// 分析端到端延迟与吞吐。
//
// 为什么用 std::atomic？
//   多线程同时读写同一个计数时，原子操作保证无数据竞争，无需加锁，
//   开销远低于 mutex。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>

namespace vision {

struct Perf {
    // 单例访问。
    static Perf& instance() {
        static Perf p;
        return p;
    }

    std::atomic<int64_t> capture_us{0};      // 采集延迟（最近一次，微秒）
    std::atomic<int64_t> inference_us{0};    // 推理延迟（最近一次，微秒）
    std::atomic<int64_t> encode_us{0};       // 编码延迟（最近一次，微秒）
    std::atomic<int64_t> total_frames{0};    // 累计编码帧数
    std::atomic<int64_t> dropped_frames{0};  // 累计丢帧数（队列满丢弃）
    std::atomic<int64_t> pushed_frames{0};   // 累计推流包数
    std::atomic<int64_t> recorded_frames{0}; // 累计录制包数
};

} // namespace vision
