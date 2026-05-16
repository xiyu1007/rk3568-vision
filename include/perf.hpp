#pragma once

#include "types.hpp"
#include "fps.h"

#include <atomic>

namespace rk3568_vision {

struct PerfStats {
    std::atomic<int64_t> capture_latency_us{0};
    std::atomic<int64_t> infer_latency_us{0};
    std::atomic<int64_t> encode_latency_us{0};
    std::atomic<int64_t> total_latency_us{0};
    std::atomic<int64_t> dropped_frames{0};
    std::atomic<int64_t> total_frames{0};
    std::atomic<int64_t> last_report_time{0};

    void report(int64_t now_ms);
};

void perf_record_capture(Timestamp start, Timestamp end);
void perf_record_infer(Timestamp start, Timestamp end);
void perf_record_encode(Timestamp start, Timestamp end);
void perf_record_total(Timestamp start, Timestamp end);
void perf_record_drop();
PerfStats& perf_stats();

} // namespace rk3568_vision
