// ============================================================================
// frame_pacer.cpp — 稳帧器实现
// ============================================================================

#include "frame_pacer.hpp"

#include "logger.hpp"

namespace vision {

FramePacer::~FramePacer() {
    stop();
}

// ---------------------------------------------------------------------------
// start：启动线程
// ---------------------------------------------------------------------------
void FramePacer::start(RingBuffer<FramePtr>* in, RingBuffer<FramePtr>* out,
                       const PacerConfig& cfg) {
    in_  = in;
    out_ = out;
    cfg_ = cfg;
    running_.store(true);
    thread_ = std::thread(&FramePacer::run, this);
}

// ---------------------------------------------------------------------------
// stop：停止线程
// ---------------------------------------------------------------------------
void FramePacer::stop() {
    if (!running_.exchange(false)) {
        return;   // 本来就没在跑
    }
    // 关闭输入队列会唤醒阻塞中的 popFor，使线程退出。
    in_->close();
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO("pacer: stopped");
}

// ---------------------------------------------------------------------------
// run：主循环
// ---------------------------------------------------------------------------
void FramePacer::run() {
    using clock = std::chrono::steady_clock;

    const uint32_t fps = (cfg_.target_fps > 0) ? cfg_.target_fps : 25;
    // 每个输出帧的时间间隔（纳秒精度）。
    const auto period = std::chrono::nanoseconds(1000000000LL / fps);

    // 下一次输出的时间点（先等一个周期再输出首帧）。
    auto next_emit = clock::now() + period;

    FramePtr last;              // 私有副本：仅用于补帧，不与下游共享
    bool     have_last = false; // 是否已有可补帧的上一帧

    while (running_.load()) {
        // ---- 1. 等待新帧，最多等到本周期该输出的时刻 ----
        auto now = clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_emit - now);
        if (wait.count() < 0) wait = std::chrono::milliseconds(0);

        FramePtr fresh;
        bool got = in_->popFor(fresh, wait);

        if (got) {
            // 新帧到达：更新私有副本，同时该帧将作为本周期输出。
            last      = cloneFrame(fresh);   // 深拷贝，隔离下游突变
            have_last = true;
        }

        // ---- 2. 到达输出时刻则输出一帧 ----
        now = clock::now();
        if (now >= next_emit) {
            FramePtr out;
            if (got) {
                out = fresh;                     // 本周期有新帧，直接输出
            } else if (have_last && cfg_.allow_duplicate) {
                out = cloneFrame(last);          // 补帧：复制上一帧
            }
            // 否则（无帧可补 或 不允许补帧）本周期跳过，不输出

            if (out) {
                out_->push(out);                 // 丢最旧策略入队
            }

            // 推进到下一个周期；若已严重落后则直接对齐当前时间，避免追帧。
            next_emit += period;
            if (next_emit <= clock::now()) {
                next_emit = clock::now() + period;
            }
        }
    }
}

} // namespace vision
