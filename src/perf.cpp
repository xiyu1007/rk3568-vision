#include "perf.hpp"
#include "logger.hpp"
#include <algorithm>

namespace rk3568_vision {

static PerfStats g_perf;
PerfStats& perf_stats() { return g_perf; }

void PerfStats::report(int64_t now_ms) {
    int64_t total = total_frames.load(std::memory_order_relaxed);
    int64_t drops = dropped_frames.load(std::memory_order_relaxed);
    if (total == 0) return;
    float cap_ms = capture_latency_us.load() * 0.001f;
    float inf_ms = infer_latency_us.load()   * 0.001f;
    float enc_ms = encode_latency_us.load()  * 0.001f;
    float drop_rate = (total > 0) ? (100.0f * drops / (total + drops)) : 0.0f;
    LOG_INFO("PERF: frames=%ld drops=%ld(%.1f%%) cap=%.1fms inf=%.1fms enc=%.1fms",
             total, drops, drop_rate, cap_ms, inf_ms, enc_ms);
    capture_latency_us = 0; infer_latency_us = 0; encode_latency_us = 0;
}

static int64_t elapsed_us(Timestamp start, Timestamp end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

void perf_record_capture(Timestamp s, Timestamp e) {
    g_perf.capture_latency_us.store(elapsed_us(s, e), std::memory_order_relaxed);
}
void perf_record_infer(Timestamp s, Timestamp e) {
    g_perf.infer_latency_us.store(elapsed_us(s, e), std::memory_order_relaxed);
}
void perf_record_encode(Timestamp s, Timestamp e) {
    g_perf.encode_latency_us.store(elapsed_us(s, e), std::memory_order_relaxed);
}
void perf_record_total(Timestamp s, Timestamp e) {
    g_perf.total_latency_us.store(elapsed_us(s, e), std::memory_order_relaxed);
    g_perf.total_frames.fetch_add(1, std::memory_order_relaxed);
}
void perf_record_drop() {
    g_perf.dropped_frames.fetch_add(1, std::memory_order_relaxed);
}
} // namespace rk3568_vision
