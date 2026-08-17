// ============================================================================
// pipeline.cpp — 流水线协调器实现
// ============================================================================

#include "vision/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "vision/debug.hpp"
#include "vision/logger.hpp"

namespace vision {

namespace {

// 读 /proc/stat 第一行，返回 CPU 累计 total/idle（用于计算 CPU 使用率）。
bool ReadCpuStat(uint64_t& total, uint64_t& idle) {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
        return false;
    }
    std::string line;
    std::getline(file, line);
    unsigned long long user = 0, nice = 0, system = 0, idle_time = 0, iowait = 0;
    if (std::sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu",
                    &user, &nice, &system, &idle_time, &iowait) < 4) {
        return false;
    }
    total = user + nice + system + idle_time + iowait;
    idle  = idle_time + iowait;
    return true;
}

// 读 /proc/meminfo，返回内存使用率（0~100）。
bool ReadMemoryUsage(double& usage) {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        return false;
    }
    uint64_t total = 0, available = 0;
    std::string line;
    while (std::getline(file, line)) {
        char key[64];
        unsigned long long value = 0;
        if (std::sscanf(line.c_str(), "%63s %llu", key, &value) < 2) {
            continue;
        }
        if (std::strcmp(key, "MemTotal:") == 0) {
            total = value;
        } else if (std::strcmp(key, "MemAvailable:") == 0) {
            available = value;
        }
    }
    if (total == 0) {
        return false;
    }
    usage = 100.0 * (1.0 - static_cast<double>(available) / total);
    return true;
}

// 读 thermal_zone 温度（毫摄氏度 → 摄氏度）。
bool ReadTemperature(double& temperature) {
    const char* zones[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp"};
    for (const char* zone : zones) {
        std::ifstream file(zone);
        if (!file.is_open()) {
            continue;
        }
        long milli_degrees = 0;
        if (file >> milli_degrees) {
            temperature = static_cast<double>(milli_degrees) / 1000.0;
            return true;
        }
    }
    return false;
}

// 深拷贝一帧（稳帧器补帧用，避免与下游共享同一块 NV12 造成竞争）。
// 对零拷贝帧（V4L2 mmap）会拷贝到 CPU 内存；对 mp4 帧直接拷贝 CPU 内存。
FramePtr CloneFrame(const FramePtr& source) {
    auto copy = std::make_shared<Frame>();
    copy->sequence = source->sequence;
    copy->width = source->width;
    copy->height = source->height;
    copy->capture_timestamp = source->capture_timestamp;
    copy->nv12_stride = source->nv12_stride;

    // 深拷贝 NV12（stride × height × 1.5 字节）。
    auto nv12_cpu = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(source->nv12_stride) * source->height * 3 / 2);
    std::memcpy(nv12_cpu->data(), source->nv12_data,
                static_cast<size_t>(source->nv12_stride) * source->height * 3 / 2);
    copy->nv12_data = nv12_cpu->data();
    copy->nv12_cpu = std::move(nv12_cpu);
    return copy;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Pipeline::Pipeline(const Config& config) : config_(config) {}

Pipeline::~Pipeline() {
    Stop();
}

// ---------------------------------------------------------------------------
// Initialize：打开采集/推理/编码，注册回调
// ---------------------------------------------------------------------------
bool Pipeline::Initialize() {
    // 1. 打开采集（source=v4l2 摄像头 / mp4 文件）。
    if (!camera_source_.Initialize(config_.capture)) {
        Logger::instance().error("pipeline: camera source init failed");
        return false;
    }
    width_ = camera_source_.GetWidth();
    height_ = camera_source_.GetHeight();
    fps_ = camera_source_.GetFrameRate();

    // 2. 注册采集回调（回调解耦：回调里只入队，不做重活）。
    camera_source_.RegisterFrameCallback(
        [this](FramePtr frame) { OnFrameCaptured(std::move(frame)); });

    // 3. 创建推理器（失败降级为无推理，仅编码推流）。
    if (config_.inference.enabled) {
        inferencer_ = std::make_unique<Inferencer>(config_.inference);
        if (!inferencer_->Initialize()) {
            Logger::instance().warn("pipeline: inference init failed, running without detection");
            inferencer_.reset();
        }
    }

    // 4. 打开编码器（硬编优先，失败回退软编）。
    if (config_.encode.hardware) {
        mpp_encoder_ = std::make_unique<MppEncoder>();
        if (mpp_encoder_->Open(config_.encode, width_, height_, fps_)) {
            use_hardware_ = true;
        } else {
            Logger::instance().warn("pipeline: MPP hardware encoder open failed, fallback to libx264");
            mpp_encoder_.reset();
        }
    }
    if (!use_hardware_ && !encoder_.Open(config_.encode, width_, height_, fps_)) {
        Logger::instance().error("pipeline: encoder open failed");
        return false;
    }

    Logger::instance().info("pipeline: initialized %ux%u@%u encoder=%s inference=%s",
                            width_, height_, fps_,
                            use_hardware_ ? "hw(mpp)" : "sw(libx264)",
                            inferencer_ ? "on" : "off");
    return true;
}

// ---------------------------------------------------------------------------
// Start：启动采集流 + 创建所有工作线程
// ---------------------------------------------------------------------------
bool Pipeline::Start() {
    if (!camera_source_.Start()) {
        Logger::instance().error("pipeline: camera source start failed");
        return false;
    }
    running_.store(true);

    pacer_thread_     = std::thread(&Pipeline::PacerLoop, this);
    inference_thread_ = std::thread(&Pipeline::InferenceLoop, this);
    encode_thread_    = std::thread(&Pipeline::EncodeLoop, this);
    if (config_.stream.enabled) {
        push_thread_ = std::thread(&Pipeline::PushLoop, this);
    }
    if (config_.record.enabled) {
        record_thread_ = std::thread(&Pipeline::RecordLoop, this);
    }
    monitor_thread_   = std::thread(&Pipeline::MonitorLoop, this);

    Logger::instance().info("pipeline: started");
    return true;
}

// ---------------------------------------------------------------------------
// Stop：优雅停止 + 释放资源（幂等）
// ---------------------------------------------------------------------------
void Pipeline::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // 1. 停止采集（唤醒采集线程）。
    camera_source_.Stop();

    // 2. 关闭上游队列，唤醒稳帧/推理/编码线程。
    capture_queue_.Close();
    inference_queue_.Close();
    encode_queue_.Close();
    if (pacer_thread_.joinable())     { pacer_thread_.join(); }
    if (inference_thread_.joinable()) { inference_thread_.join(); }
    if (encode_thread_.joinable())    { encode_thread_.join(); }  // 编码线程冲刷残留帧

    // 3. 关闭下游队列，唤醒推流/录制/监控线程。
    push_queue_.Close();
    record_queue_.Close();
    if (push_thread_.joinable())   { push_thread_.join(); }
    if (record_thread_.joinable()) { record_thread_.join(); }
    if (monitor_thread_.joinable()) { monitor_thread_.join(); }

    // 4. 释放资源。
    if (use_hardware_) { mpp_encoder_->Close(); } else { encoder_.Close(); }
    if (rtmp_streamer_) { rtmp_streamer_->Close(); }
    if (mp4_recorder_)  { mp4_recorder_->Close(); }
    Logger::instance().info("pipeline: stopped");
}

// ---------------------------------------------------------------------------
// OnFrameCaptured：采集回调（只入队，不做重活）
// ---------------------------------------------------------------------------
void Pipeline::OnFrameCaptured(FramePtr frame) {
    const uint64_t sequence = frame_sequence_++;
    frame->sequence = sequence;
    capture_queue_.Push(std::move(frame));
    VISION_DEBUG_LOG("capture: frame seq=%llu queue=%zu",
                     static_cast<unsigned long long>(sequence),
                     capture_queue_.Size());
}

// ===========================================================================
//  工作线程
// ===========================================================================

// ---------------------------------------------------------------------------
// PacerLoop：稳帧器，capture_queue_ → inference_queue_
// ---------------------------------------------------------------------------
void Pipeline::PacerLoop() {
    using Clock = std::chrono::steady_clock;
    const uint32_t fps = config_.pacer.target_fps > 0 ? config_.pacer.target_fps : 25;
    const auto period = std::chrono::nanoseconds(1000000000LL / fps);
    auto next_emit = Clock::now() + period;

    FramePtr last_frame;
    bool has_last = false;

    while (running_.load()) {
        auto now = Clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_emit - now);
        if (wait.count() < 0) {
            wait = std::chrono::milliseconds(0);
        }

        FramePtr fresh_frame;
        const bool got = capture_queue_.PopFor(fresh_frame, wait);
        if (got) {
            last_frame = CloneFrame(fresh_frame);   // 私有副本，隔离下游突变
            has_last = true;
        }

        now = Clock::now();
        if (now >= next_emit) {
            FramePtr output;
            if (got) {
                output = fresh_frame;                             // 有新帧直接输出
            } else if (has_last && config_.pacer.allow_duplicate) {
                output = CloneFrame(last_frame);                  // 补帧
            }
            if (output) {
                inference_queue_.Push(std::move(output));
            }

            next_emit += period;                                 // 推进到下个周期
            if (next_emit <= Clock::now()) {
                next_emit = Clock::now() + period;               // 防追帧
            }
        }
    }
}

// ---------------------------------------------------------------------------
// InferenceLoop：inference_queue_ → 推理 → encode_queue_
// ---------------------------------------------------------------------------
void Pipeline::InferenceLoop() {
    while (running_.load()) {
        FramePtr frame;
        if (!inference_queue_.Pop(frame)) {
            break;
        }
        if (inferencer_) {
            const uint64_t start_us = GetCurrentTimestampUs();
            inferencer_->Detect(frame);
            perf_stats_.inference_us.store(GetCurrentTimestampUs() - start_us);
        }
        encode_queue_.Push(std::move(frame));
    }
}

// ---------------------------------------------------------------------------
// EncodeLoop：encode_queue_ → 画框 + 编码 → push_queue_/record_queue_
// ---------------------------------------------------------------------------
void Pipeline::EncodeLoop() {
    while (running_.load()) {
        FramePtr frame;
        if (!encode_queue_.Pop(frame)) {
            break;
        }
        // 画检测框供远程展示（就地修改 NV12）。
        DrawBoxesOnNv12(const_cast<uint8_t*>(frame->nv12_data),
                        static_cast<int>(frame->width),
                        static_cast<int>(frame->height),
                        static_cast<int>(frame->nv12_stride),
                        frame->detection);

        const uint64_t start_us = GetCurrentTimestampUs();
        if (use_hardware_) {
            mpp_encoder_->Encode(frame, [this](const PacketPtr& packet) { DispatchPacket(packet); });
        } else {
            encoder_.Encode(frame, [this](const PacketPtr& packet) { DispatchPacket(packet); });
        }
        perf_stats_.encode_us.store(GetCurrentTimestampUs() - start_us);
        perf_stats_.total_frames.fetch_add(1);
    }
    // 冲刷编码器残留帧。
    if (use_hardware_) {
        mpp_encoder_->Flush([this](const PacketPtr& packet) { DispatchPacket(packet); });
    } else {
        encoder_.Flush([this](const PacketPtr& packet) { DispatchPacket(packet); });
    }
}

// ---------------------------------------------------------------------------
// DispatchPacket：编码包分发（首帧后打开推流/录制）
// ---------------------------------------------------------------------------
void Pipeline::DispatchPacket(const PacketPtr& packet) {
    // 首次收到编码包时，编码器的 SPS/PPS(extradata) 才就绪，此时打开封装器。
    if (!extradata_ready_) {
        extradata_ready_ = true;
        const uint8_t* ed = use_hardware_ ? mpp_encoder_->GetExtradata()
                                          : encoder_.GetExtradata();
        const int ed_size = use_hardware_ ? mpp_encoder_->GetExtradataSize()
                                          : encoder_.GetExtradataSize();
        if (ed != nullptr && ed_size > 0) {
            extradata_.assign(ed, ed + ed_size);
        }
        Logger::instance().info("encoder extradata(SPS/PPS) size: %zu", extradata_.size());

        if (config_.stream.enabled) {
            rtmp_streamer_ = std::make_unique<RtmpStreamer>(config_.stream);
            rtmp_streamer_->SetVideoParameters(width_, height_, fps_, config_.encode.bitrate,
                                               extradata_.empty() ? nullptr : extradata_.data(),
                                               static_cast<int>(extradata_.size()));
        }
        if (config_.record.enabled) {
            mp4_recorder_ = std::make_unique<Mp4Recorder>(config_.record);
            mp4_recorder_->Open(width_, height_, fps_, config_.encode.bitrate,
                                extradata_.empty() ? nullptr : extradata_.data(),
                                static_cast<int>(extradata_.size()));
        }
    }
    // 推流/录制在各自线程的 Push 里完成打开（rtmp_streamer 延迟到首包 Open）。
    if (config_.stream.enabled && rtmp_streamer_) {
        push_queue_.Push(packet);
    }
    if (config_.record.enabled && mp4_recorder_) {
        record_queue_.Push(packet);
    }
}

// ---------------------------------------------------------------------------
// PushLoop：push_queue_ → RTMP（断线重连由 RtmpStreamer 内部处理）
// ---------------------------------------------------------------------------
void Pipeline::PushLoop() {
    while (running_.load()) {
        PacketPtr packet;
        if (!push_queue_.Pop(packet)) {
            break;
        }
        if (rtmp_streamer_ && rtmp_streamer_->Push(packet)) {
            perf_stats_.pushed_frames.fetch_add(1);
        }
    }
}

// ---------------------------------------------------------------------------
// RecordLoop：record_queue_ → MP4
// ---------------------------------------------------------------------------
void Pipeline::RecordLoop() {
    while (running_.load()) {
        PacketPtr packet;
        if (!record_queue_.Pop(packet)) {
            break;
        }
        if (mp4_recorder_ && mp4_recorder_->Push(packet)) {
            perf_stats_.recorded_frames.fetch_add(1);
        }
    }
}

// ---------------------------------------------------------------------------
// MonitorLoop：周期打印系统状态
// ---------------------------------------------------------------------------
void Pipeline::MonitorLoop() {
    while (running_.load()) {
        for (int wait = 0;
             wait < static_cast<int>(config_.monitor_interval_ms) && running_.load();
             wait += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!running_.load()) {
            break;
        }

        uint64_t total = 0, idle = 0;
        double cpu = 0, memory = 0, temperature = 0;
        if (ReadCpuStat(total, idle) && last_cpu_total_ != 0) {
            const uint64_t delta_total = total - last_cpu_total_;
            const uint64_t delta_idle = idle - last_cpu_idle_;
            if (delta_total > 0) {
                cpu = 100.0 * (1.0 - static_cast<double>(delta_idle) / delta_total);
            }
        }
        last_cpu_total_ = total;
        last_cpu_idle_ = idle;
        ReadMemoryUsage(memory);
        ReadTemperature(temperature);

        Logger::instance().info(
            "monitor: cpu=%.1f%% mem=%.1f%% temp=%.1fC | "
            "enc=%lldms inf=%lldms | frames=%lld drop=%lld push=%lld rec=%lld",
            cpu, memory, temperature,
            static_cast<long long>(perf_stats_.encode_us.load()) / 1000,
            static_cast<long long>(perf_stats_.inference_us.load()) / 1000,
            static_cast<long long>(perf_stats_.total_frames.load()),
            static_cast<long long>(perf_stats_.dropped_frames.load()),
            static_cast<long long>(perf_stats_.pushed_frames.load()),
            static_cast<long long>(perf_stats_.recorded_frames.load()));
    }
}

} // namespace vision
