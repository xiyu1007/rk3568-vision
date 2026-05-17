#include "perf.h"
#include "logger.h"

perf_t g_perf = {0};

void perf_record_capture(int64_t us) { g_perf.cap_us = us; }
void perf_record_infer(int64_t us)   { g_perf.inf_us = us; }
void perf_record_encode(int64_t us)  { g_perf.enc_us = us; }

void perf_record_drop(void) {
    __atomic_fetch_add(&g_perf.dropped_frames, 1, __ATOMIC_RELAXED);
}

void perf_report(void) {
    int64_t total = __atomic_load_n(&g_perf.total_frames, __ATOMIC_RELAXED);
    int64_t drops = __atomic_load_n(&g_perf.dropped_frames, __ATOMIC_RELAXED);
    if (total == 0) return;

    float cap_ms = (float)__atomic_load_n(&g_perf.cap_us, __ATOMIC_RELAXED) * 0.001f;
    float inf_ms = (float)__atomic_load_n(&g_perf.inf_us, __ATOMIC_RELAXED) * 0.001f;
    float enc_ms = (float)__atomic_load_n(&g_perf.enc_us, __ATOMIC_RELAXED) * 0.001f;
    float drop_rate = (total + drops > 0) ? (100.0f * (float)drops / (float)(total + drops)) : 0.0f;

    LOG_INFO("PERF: frames=%ld drops=%ld(%.1f%%) cap=%.1fms inf=%.1fms enc=%.1fms",
             (long)total, (long)drops, drop_rate, cap_ms, inf_ms, enc_ms);
}
