// ============================================================================
// pipeline.cpp — 流水线编排实现
// ============================================================================

#include "pipeline.hpp"

#include <chrono>

#include "image_utils.hpp"
#include "logger.hpp"
#include "perf.hpp"

namespace vision {

namespace {
// 各队列深度（有界，避免内存/延迟无限增长）。
constexpr size_t kCapQueueDepth    = 8;    // 采集→稳帧
constexpr size_t kInfQueueDepth    = 8;    // 稳帧→推理
constexpr size_t kEncQueueDepth    = 8;    // 推理→编码
constexpr size_t kPushQueueDepth   = 16;   // 编码→推流
constexpr size_t kRecordQueueDepth = 64;   // 编码→录制（允许更多缓冲）
} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Pipeline::Pipeline(const Config& cfg)
    : cfg_(cfg),
      cap_q_(kCapQueueDepth),
      inf_q_(kInfQueueDepth),
      enc_q_(kEncQueueDepth),
      push_q_(kPushQueueDepth),
      record_q_(kRecordQueueDepth) {}

Pipeline::~Pipeline() {
    stop();
}

// ---------------------------------------------------------------------------
// init：初始化各模块
// ---------------------------------------------------------------------------
bool Pipeline::init() {
    // 1. 打开采集（失败则无法继续）。
    if (!capture_.open(cfg_.capture)) {
        LOG_ERROR("pipeline: capture open failed");
        return false;
    }
    width_  = capture_.width();
    height_ = capture_.height();
    fps_    = capture_.fps();

    // 2. 创建推理器（失败则降级为无推理，仅编码推流）。
    if (cfg_.inference.enabled) {
        inferencer_ = createInferencer(cfg_);
        if (!inferencer_ || !inferencer_->init()) {
            LOG_WARN("pipeline: inference init failed, running without detection");
            inferencer_.reset();
        }
    }

    // 3. 打开编码器（失败则无法继续）。
    if (!cfg_.encode.enabled) {
        LOG_ERROR("pipeline: encode disabled, nothing to do");
        return false;
    }
    if (!encoder_.open(cfg_.encode, width_, height_, fps_)) {
        LOG_ERROR("pipeline: encoder open failed");
        return false;
    }

    // 4. 缓存 extradata（SPS/PPS），供推流/录制/重连使用。
    if (encoder_.extradata() && encoder_.extradata_size() > 0) {
        extradata_.assign(encoder_.extradata(),
                          encoder_.extradata() + encoder_.extradata_size());
    }

    // 5. 打开推流与录制封装器（失败仅告警，不阻断）。
    if (cfg_.stream.enabled) {
        if (!openRtmp()) LOG_WARN("pipeline: RTMP open failed, streaming disabled");
    }
    if (cfg_.record.enabled) {
        if (!openMp4()) LOG_WARN("pipeline: MP4 open failed, recording disabled");
    }

    LOG_INFO("pipeline: initialized (%ux%u@%u, encoder=%s, inference=%s)",
             width_, height_, fps_,
             encoder_.hardware() ? "hw" : "sw",
             inferencer_ ? inferencer_->name() : "disabled");
    return true;
}

// ---------------------------------------------------------------------------
// openRtmp / openMp4
// ---------------------------------------------------------------------------
bool Pipeline::openRtmp() {
    return rtmp_muxer_.open("flv", cfg_.stream.url, width_, height_, fps_,
                            cfg_.encode.bitrate,
                            extradata_.empty() ? nullptr : extradata_.data(),
                            static_cast<int>(extradata_.size()));
}

bool Pipeline::openMp4() {
    return mp4_muxer_.open("mp4", cfg_.record.path, width_, height_, fps_,
                           cfg_.encode.bitrate,
                           extradata_.empty() ? nullptr : extradata_.data(),
                           static_cast<int>(extradata_.size()));
}

// ---------------------------------------------------------------------------
// start：启动采集流与所有线程
// ---------------------------------------------------------------------------
bool Pipeline::start() {
    if (!capture_.start()) {
        LOG_ERROR("pipeline: capture start failed");
        return false;
    }
    running_.store(true);

    // 依次创建线程（采集 → 稳帧 → 推理 → 编码 → 推流 → 录制）。
    capture_thread_   = std::thread(&Pipeline::captureLoop, this);
    pacer_.start(&cap_q_, &inf_q_, cfg_.pacer);
    inference_thread_ = std::thread(&Pipeline::inferenceLoop, this);
    encode_thread_    = std::thread(&Pipeline::encodeLoop, this);

    if (cfg_.stream.enabled) push_thread_ = std::thread(&Pipeline::pushLoop, this);
    if (cfg_.record.enabled) record_thread_ = std::thread(&Pipeline::recordLoop, this);

    LOG_INFO("pipeline: started");
    return true;
}

// ---------------------------------------------------------------------------
// stop：优雅停止
// ---------------------------------------------------------------------------
void Pipeline::stop() {
    if (!running_.exchange(false)) return;   // 已停止

    // 1. 停止稳帧器（关闭其输入队列并 join）。
    pacer_.stop();

    // 2. 关闭上游帧队列，唤醒采集/推理/编码线程。
    cap_q_.close();
    inf_q_.close();
    enc_q_.close();

    // 3. 先 join 编码线程（它会冲刷编码器，把残留包交给仍在运行的推流/录制线程，
    //    保证 MP4 收尾完整）。
    if (capture_thread_.joinable())   capture_thread_.join();
    if (inference_thread_.joinable()) inference_thread_.join();
    if (encode_thread_.joinable())    encode_thread_.join();

    // 4. 编码完成后，关闭下游包队列，唤醒推流/录制线程。
    push_q_.close();
    record_q_.close();
    if (push_thread_.joinable())      push_thread_.join();
    if (record_thread_.joinable())    record_thread_.join();

    // 5. 停止采集、释放编码器与封装器。
    capture_.stop();
    encoder_.close();
    rtmp_muxer_.close();
    mp4_muxer_.close();

    LOG_INFO("pipeline: stopped");
}

// ---------------------------------------------------------------------------
// captureLoop：采集 → cap_q
// ---------------------------------------------------------------------------
void Pipeline::captureLoop() {
    while (running_.load()) {
        FramePtr f;
        uint64_t t0 = nowUs();
        if (!capture_.read(f)) {
            continue;   // poll 超时，继续循环（顺便检查 running_）
        }
        Perf::instance().capture_us.store(nowUs() - t0);
        f->seq = seq_++;
        cap_q_.push(f);
    }
}

// ---------------------------------------------------------------------------
// inferenceLoop：inf_q → 推理 → enc_q
// ---------------------------------------------------------------------------
void Pipeline::inferenceLoop() {
    while (running_.load()) {
        FramePtr f;
        if (!inf_q_.pop(f)) break;   // 队列关闭

        if (inferencer_) {
            uint64_t t0 = nowUs();
            inferencer_->detect(f);
            Perf::instance().inference_us.store(nowUs() - t0);
        }
        enc_q_.push(f);
    }
}

// ---------------------------------------------------------------------------
// encodeLoop：enc_q → 画框 + 编码 → push_q / record_q
// ---------------------------------------------------------------------------
void Pipeline::encodeLoop() {
    while (running_.load()) {
        FramePtr f;
        if (!enc_q_.pop(f)) break;

        // 1. 把检测框画到 NV12 上（远程展示用）。
        drawBoxesNv12(f->nv12.data(), f->width, f->height, f->detect);

        // 2. 编码（可能一次吐出多个包）。
        uint64_t t0 = nowUs();
        encoder_.encode(f, [this](const PacketPtr& pkt) {
            dispatchPacket(pkt);
        });
        Perf::instance().encode_us.store(nowUs() - t0);
        Perf::instance().total_frames.fetch_add(1);
    }
    // 退出前冲刷编码器残留帧。
    encoder_.flush([this](const PacketPtr& pkt) { dispatchPacket(pkt); });
}

// ---------------------------------------------------------------------------
// dispatchPacket：编码包分发到推流/录制队列
// ---------------------------------------------------------------------------
void Pipeline::dispatchPacket(const PacketPtr& pkt) {
    if (cfg_.stream.enabled) push_q_.push(pkt);
    if (cfg_.record.enabled) record_q_.push(pkt);
}

// ---------------------------------------------------------------------------
// pushLoop：push_q → RTMP 推流（含断线重连）
// ---------------------------------------------------------------------------
void Pipeline::pushLoop() {
    int reconnect_count = 0;
    while (running_.load()) {
        PacketPtr pkt;
        if (!push_q_.pop(pkt)) break;

        // 连接已断开且允许重连 → 延迟后重建。
        if (!rtmp_muxer_.isOpen()) {
            if (cfg_.stream.reconnect &&
                (cfg_.stream.max_reconnect < 0 ||
                 reconnect_count < cfg_.stream.max_reconnect)) {
                LOG_WARN("push: reconnecting (%d)...", reconnect_count + 1);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(cfg_.stream.reconnect_delay_ms));
                openRtmp();
                ++reconnect_count;
            } else {
                continue;   // 放弃重连，丢弃该包
            }
        }

        if (rtmp_muxer_.push(pkt)) {
            Perf::instance().pushed_frames.fetch_add(1);
            reconnect_count = 0;   // 成功则重置计数
        } else {
            rtmp_muxer_.close();   // 标记断开，下次循环重连
        }
    }
}

// ---------------------------------------------------------------------------
// recordLoop：record_q → MP4 录制
// ---------------------------------------------------------------------------
void Pipeline::recordLoop() {
    while (running_.load()) {
        PacketPtr pkt;
        if (!record_q_.pop(pkt)) break;
        if (mp4_muxer_.isOpen() && mp4_muxer_.push(pkt)) {
            Perf::instance().recorded_frames.fetch_add(1);
        }
    }
}

} // namespace vision
