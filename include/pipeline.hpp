#pragma once

#include "config.hpp"
#include "types.hpp"
#include "ring_buffer.hpp"
#include "v4l2_capture.hpp"
#include "detector.hpp"
#include "ffmpeg_encoder.hpp"
#include "rtmp_pusher.hpp"
#include "display.hpp"

#include <memory>
#include <atomic>
#include <thread>
#include <vector>

namespace rk3568_vision {

// ============================================================================
// Pipeline — 多线程视频处理管线
//
// 线程模型 (4+N 线程):
//   capture   线程: V4L2 DQBUF -> FrameBuffer -> capture_queue_.push()
//   inference 线程: capture_queue_.pop() -> Detector::detect() -> infer_queue_.push()
//   encode    线程: infer_queue_.pop() -> FFmpegEncoder::encode() -> RTMP push
//   display   线程: infer_queue_.pop() -> OSD叠加 -> cv::imshow()
//
// 队列容量: 4-8帧（1080P@30fps下，每帧~3MB NV12，队列内存<32MB）
//
// 丢帧策略: 队列满时丢弃最旧帧（而非阻塞生产者）
// ============================================================================
class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool init(const CaptureConfig& cap_cfg, const InferenceConfig& inf_cfg,
              const EncodeConfig& enc_cfg, const StreamConfig& strm_cfg,
              const DisplayConfig& disp_cfg);
    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    void capture_loop();
    void inference_loop();
    void encode_loop();
    void display_loop();

    // 配置
    CaptureConfig   cap_cfg_;
    InferenceConfig inf_cfg_;
    EncodeConfig    enc_cfg_;
    StreamConfig    strm_cfg_;
    DisplayConfig   disp_cfg_;

    // 模块实例
    std::unique_ptr<V4L2Capture>   capture_;
    std::unique_ptr<Detector>      detector_;
    std::unique_ptr<FFmpegEncoder> encoder_;
    std::unique_ptr<RTMPPusher>    streamer_;
    std::unique_ptr<Display>       display_;

    // 线程间队列
    // 队列1: 采集 -> 推理 (传递原始帧 NV12)
    RingBuffer<std::shared_ptr<FrameBuffer>> capture_queue_{8};
    // 队列2: 推理/采集 -> 编码+显示 (传递带检测结果的帧)
    RingBuffer<std::shared_ptr<FrameBuffer>> infer_queue_{8};

    // 工作线程
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

} // namespace rk3568_vision
