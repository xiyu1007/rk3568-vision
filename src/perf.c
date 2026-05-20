/*
 * ==========================================================================
 * perf.c — 性能统计模块（Performance Counters）
 * ==========================================================================
 *
 * **功能**：记录和报告各处理阶段的延迟数据
 *
 * **统计指标**：
 *   - cap_us：采集延迟（V4L2 DQBUF + memcpy 耗时）
 *   - inf_us：推理延迟（预处理 + NPU 推理 + 后处理总耗时）
 *   - enc_us：编码延迟（NV12→YUV420P 转换 + x264 编码耗时）
 *   - total_frames：累计处理帧数
 *   - dropped_frames：累计丢帧数（环形队列满时丢弃）
 *
 * **为什么使用原子操作？**
 *   多个线程同时写入 perf 统计（采集线程写 cap_us，推理线程写 inf_us 等）
 *   使用 __atomic_* 宏保证多线程安全，无需互斥锁
 *
 * **延迟分析**：
 *   端到端延迟 = cap_us + inf_us + enc_us
 *   目标：< 500ms（实际通常 < 100ms）
 *   典型值：
 *     cap_us ≈ 1ms（V4L2 + memcpy）
 *     inf_us ≈ 25ms（YOLOv5s INT8 NPU 推理）
 *     enc_us ≈ 5ms（libx264 软件编码）
 *     总延迟 ≈ 31ms + 队列缓冲时间
 */

#include "perf.h"
#include "logger.h"

perf_t g_perf = {0};  /* 全局性能计数器单例 */

void perf_record_capture(int64_t us) { g_perf.cap_us = us; }
void perf_record_infer(int64_t us)   { g_perf.inf_us = us; }
void perf_record_encode(int64_t us)  { g_perf.enc_us = us; }

/*
 * 记录丢帧
 * 使用原子加法，多线程安全
 * RELAXED 内存序：丢帧计数不需要与其他操作建立 happens-before 关系
 */
void perf_record_drop(void) {
    __atomic_fetch_add(&g_perf.dropped_frames, 1, __ATOMIC_RELAXED);
}

/*
 * 输出性能报告（由 monitor 线程定期调用）
 *
 * 报告内容：
 *   - frames：累计处理帧数
 *   - drops：累计丢帧数及丢帧率
 *   - cap/inf/enc：各阶段延迟（毫秒）
 *
 * 丢帧率 = drops / (total + drops) × 100%
 * 理想丢帧率 < 1%（表示系统吞吐足够处理 30fps 输入）
 *
 * 注意：
 *   - total_frames 当前未被自动更新（TODO），需要各线程手动维护
 *   - 对于延迟分析，focus 在 cap/inf/enc 的实时值上
 */
void perf_report(void) {
    int64_t total = __atomic_load_n(&g_perf.total_frames, __ATOMIC_RELAXED);
    int64_t drops = __atomic_load_n(&g_perf.dropped_frames, __ATOMIC_RELAXED);
    if (total == 0) return;  /* 没有处理过帧，跳过 */

    /* 微秒 → 毫秒 */
    float cap_ms = (float)__atomic_load_n(&g_perf.cap_us, __ATOMIC_RELAXED) * 0.001f;
    float inf_ms = (float)__atomic_load_n(&g_perf.inf_us, __ATOMIC_RELAXED) * 0.001f;
    float enc_ms = (float)__atomic_load_n(&g_perf.enc_us, __ATOMIC_RELAXED) * 0.001f;
    float drop_rate = (total + drops > 0) ? (100.0f * (float)drops / (float)(total + drops)) : 0.0f;

    LOG_INFO("PERF: frames=%ld drops=%ld(%.1f%%) cap=%.1fms inf=%.1fms enc=%.1fms",
             (long)total, (long)drops, drop_rate, cap_ms, inf_ms, enc_ms);
}
