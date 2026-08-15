// ============================================================================
// pipeline.cpp — 流水线编排实现
// ============================================================================

#include "pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "logger.hpp"

namespace vision {

namespace {

// 读 /proc/stat 第一行，返回 CPU 累计 total/idle。
bool readCpuStat(uint64_t& total, uint64_t& idle) {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return false;
    std::string line;
    std::getline(f, line);
    unsigned long long user = 0, nice = 0, sys = 0, id = 0, iowait = 0;
    if (std::sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu",
                    &user, &nice, &sys, &id, &iowait) < 4) return false;
    total = user + nice + sys + id + iowait;
    idle  = id + iowait;
    return true;
}

// 读 /proc/meminfo，返回内存使用率（0~100）。
bool readMemUsage(double& usage) {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return false;
    uint64_t total = 0, avail = 0;
    std::string line;
    while (std::getline(f, line)) {
        char key[64]; unsigned long long val = 0;
        if (std::sscanf(line.c_str(), "%63s %llu", key, &val) < 2) continue;
        if (std::strcmp(key, "MemTotal:") == 0)     total = val;
        else if (std::strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    if (total == 0) return false;
    usage = 100.0 * (1.0 - (double)avail / total);
    return true;
}

// 读 thermal_zone 温度（毫摄氏度 → 摄氏度）。
bool readTemperature(double& temp) {
    const char* zones[] = { "/sys/class/thermal/thermal_zone0/temp",
                            "/sys/class/thermal/thermal_zone1/temp" };
    for (const char* z : zones) {
        std::ifstream f(z);
        if (!f.is_open()) continue;
        long mc = 0;
        if (f >> mc) { temp = (double)mc / 1000.0; return true; }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Pipeline::Pipeline(const Config& cfg) : cfg_(cfg) {}
Pipeline::~Pipeline() { stop(); }

// ---------------------------------------------------------------------------
// init：初始化各模块
// ---------------------------------------------------------------------------
bool Pipeline::init() {
    // 1. 打开采集（source=v4l2 摄像头 / mp4 文件）。
    if (!capture_.open(cfg_.capture)) {
        LOG_ERROR("pipeline: capture open failed");
        return false;
    }
    width_ = capture_.width(); height_ = capture_.height(); fps_ = capture_.fps();

    // 2. 创建推理器（失败降级为无推理，仅编码推流）。
    if (cfg_.inference.enabled) {
        inferencer_ = std::make_unique<Inferencer>(cfg_.inference);
        if (!inferencer_->init()) {
            LOG_WARN("pipeline: inference init failed, running without detection");
            inferencer_.reset();
        }
    }

    // 3. 打开编码器。
    if (!encoder_.open(cfg_.encode, width_, height_, fps_)) {
        LOG_ERROR("pipeline: encoder open failed");
        return false;
    }
    // 封装器(RTMP/MP4)延迟到首帧编码后打开（见 dispatchPacket），
    // 因为编码器的 SPS/PPS(extradata) 需首帧编码后才生成。
    LOG_INFO("pipeline: initialized %ux%u@%u encoder=%s inference=%s",
             width_, height_, fps_, encoder_.hardware() ? "hw" : "sw",
             inferencer_ ? "on" : "off");
    return true;
}

bool Pipeline::openRtmp() {
    return rtmp_muxer_.open("flv", cfg_.stream.url, width_, height_, fps_,
                            cfg_.encode.bitrate,
                            extradata_.empty() ? nullptr : extradata_.data(),
                            (int)extradata_.size());
}

bool Pipeline::openMp4() {
    return mp4_muxer_.open("mp4", cfg_.record.path, width_, height_, fps_,
                           cfg_.encode.bitrate,
                           extradata_.empty() ? nullptr : extradata_.data(),
                           (int)extradata_.size());
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------
bool Pipeline::start() {
    if (!capture_.start()) { LOG_ERROR("pipeline: capture start failed"); return false; }
    running_.store(true);

    capture_t_   = std::thread(&Pipeline::captureLoop, this);
    pacer_t_     = std::thread(&Pipeline::pacerLoop, this);
    inference_t_ = std::thread(&Pipeline::inferenceLoop, this);
    encode_t_    = std::thread(&Pipeline::encodeLoop, this);
    if (cfg_.stream.enabled) push_t_   = std::thread(&Pipeline::pushLoop, this);
    if (cfg_.record.enabled) record_t_ = std::thread(&Pipeline::recordLoop, this);
    monitor_t_   = std::thread(&Pipeline::monitorLoop, this);

    LOG_INFO("pipeline: started");
    return true;
}

void Pipeline::stop() {
    if (!running_.exchange(false)) return;

    // 1. 关闭上游队列，唤醒采集/稳帧/推理/编码线程。
    cap_q_.close(); inf_q_.close(); enc_q_.close();
    if (capture_t_.joinable())   capture_t_.join();
    if (pacer_t_.joinable())     pacer_t_.join();
    if (inference_t_.joinable()) inference_t_.join();
    if (encode_t_.joinable())    encode_t_.join();   // 编码线程冲刷残留帧

    // 2. 关闭下游队列，唤醒推流/录制/监控线程。
    push_q_.close(); record_q_.close();
    if (push_t_.joinable())   push_t_.join();
    if (record_t_.joinable()) record_t_.join();
    if (monitor_t_.joinable()) monitor_t_.join();

    // 3. 释放资源。
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
    while (running_) {
        FramePtr f;
        uint64_t t0 = nowUs();
        if (!capture_.read(f)) continue;
        perf_.capture_us.store(nowUs() - t0);
        f->seq = seq_++;
        cap_q_.push(f);
    }
}

// ---------------------------------------------------------------------------
// pacerLoop：稳帧器，cap_q → inf_q（按目标帧率节拍输出）
// ---------------------------------------------------------------------------
void Pipeline::pacerLoop() {
    using clock = std::chrono::steady_clock;
    const uint32_t fps = cfg_.pacer.target_fps > 0 ? cfg_.pacer.target_fps : 25;
    const auto period = std::chrono::nanoseconds(1000000000LL / fps);
    auto next_emit = clock::now() + period;

    FramePtr last;
    bool have_last = false;

    while (running_) {
        auto now = clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_emit - now);
        if (wait.count() < 0) wait = std::chrono::milliseconds(0);

        FramePtr fresh;
        bool got = cap_q_.popFor(fresh, wait);
        if (got) { last = cloneFrame(fresh); have_last = true; }   // 私有副本，隔离下游突变

        now = clock::now();
        if (now >= next_emit) {
            FramePtr out;
            if (got) out = fresh;                                   // 有新帧直接输出
            else if (have_last && cfg_.pacer.allow_duplicate) out = cloneFrame(last); // 补帧
            if (out) inf_q_.push(out);

            next_emit += period;                                    // 推进到下个周期
            if (next_emit <= clock::now()) next_emit = clock::now() + period; // 防追帧
        }
    }
}

// ---------------------------------------------------------------------------
// inferenceLoop：inf_q → 推理 → enc_q
// ---------------------------------------------------------------------------
void Pipeline::inferenceLoop() {
    while (running_) {
        FramePtr f;
        if (!inf_q_.pop(f)) break;
        if (inferencer_) {
            uint64_t t0 = nowUs();
            inferencer_->detect(f);
            perf_.inference_us.store(nowUs() - t0);
        }
        enc_q_.push(f);
    }
}

// ---------------------------------------------------------------------------
// encodeLoop：enc_q → 画框 + 编码 → push_q / record_q
// ---------------------------------------------------------------------------
void Pipeline::encodeLoop() {
    while (running_) {
        FramePtr f;
        if (!enc_q_.pop(f)) break;
        drawBoxesNv12(f->nv12.data(), f->width, f->height, f->detect);   // 画框供远程展示
        uint64_t t0 = nowUs();
        encoder_.encode(f, [this](const PacketPtr& pkt) { dispatchPacket(pkt); });
        perf_.encode_us.store(nowUs() - t0);
        perf_.total_frames.fetch_add(1);
    }
    encoder_.flush([this](const PacketPtr& pkt) { dispatchPacket(pkt); });
}

void Pipeline::dispatchPacket(const PacketPtr& pkt) {
    // 首次收到编码包时，编码器的 SPS/PPS(extradata) 才就绪，此时打开封装器。
    if (!extradata_ready_) {
        extradata_ready_ = true;
        if (encoder_.extradata() && encoder_.extradata_size() > 0)
            extradata_.assign(encoder_.extradata(),
                              encoder_.extradata() + encoder_.extradata_size());
        LOG_INFO("encoder extradata(SPS/PPS) size: %zu", extradata_.size());
        if (cfg_.stream.enabled && !openRtmp())
            LOG_WARN("pipeline: RTMP open failed, streaming disabled");
        if (cfg_.record.enabled && !openMp4())
            LOG_WARN("pipeline: MP4 open failed, recording disabled");
    }
    if (cfg_.stream.enabled && rtmp_muxer_.isOpen()) push_q_.push(pkt);
    if (cfg_.record.enabled && mp4_muxer_.isOpen()) record_q_.push(pkt);
}

// ---------------------------------------------------------------------------
// pushLoop：push_q → RTMP（含断线重连）
// ---------------------------------------------------------------------------
void Pipeline::pushLoop() {
    int reconnect_count = 0;
    while (running_) {
        PacketPtr pkt;
        if (!push_q_.pop(pkt)) break;

        if (!rtmp_muxer_.isOpen()) {
            if (cfg_.stream.reconnect &&
                (cfg_.stream.max_reconnect < 0 ||
                 reconnect_count < cfg_.stream.max_reconnect)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(cfg_.stream.reconnect_delay_ms));
                openRtmp();
                ++reconnect_count;
            } else {
                continue;
            }
        }
        if (rtmp_muxer_.push(pkt)) {
            perf_.pushed_frames.fetch_add(1);
            reconnect_count = 0;
        } else {
            rtmp_muxer_.close();
        }
    }
}

// ---------------------------------------------------------------------------
// recordLoop：record_q → MP4
// ---------------------------------------------------------------------------
void Pipeline::recordLoop() {
    while (running_) {
        PacketPtr pkt;
        if (!record_q_.pop(pkt)) break;
        if (mp4_muxer_.isOpen() && mp4_muxer_.push(pkt))
            perf_.recorded_frames.fetch_add(1);
    }
}

// ---------------------------------------------------------------------------
// monitorLoop：周期打印系统状态
// ---------------------------------------------------------------------------
void Pipeline::monitorLoop() {
    while (running_) {
        for (int w = 0; w < (int)cfg_.monitor_interval_ms && running_; w += 200)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!running_) break;

        uint64_t total = 0, idle = 0;
        double cpu = 0, mem = 0, temp = 0;
        if (readCpuStat(total, idle) && last_cpu_total_ != 0) {
            uint64_t dt = total - last_cpu_total_, di = idle - last_cpu_idle_;
            if (dt > 0) cpu = 100.0 * (1.0 - (double)di / dt);
        }
        last_cpu_total_ = total; last_cpu_idle_ = idle;
        readMemUsage(mem);
        readTemperature(temp);

        LOG_INFO("monitor: cpu=%.1f%% mem=%.1f%% temp=%.1fC | "
                 "enc=%lldms inf=%lldms cap=%lldms | "
                 "frames=%lld drop=%lld push=%lld rec=%lld",
                 cpu, mem, temp,
                 (long long)perf_.encode_us.load() / 1000,
                 (long long)perf_.inference_us.load() / 1000,
                 (long long)perf_.capture_us.load() / 1000,
                 (long long)perf_.total_frames.load(),
                 (long long)perf_.dropped_frames.load(),
                 (long long)perf_.pushed_frames.load(),
                 (long long)perf_.recorded_frames.load());
    }
}

} // namespace vision
