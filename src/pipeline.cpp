#include "fps.h"
#include <opencv2/imgproc.hpp>
#include "pipeline.hpp"
#include "logger.hpp"
#include "sig_handler.h"
#include "perf.hpp"
#include "fps.h"
#include <chrono>
#include <thread>

namespace rk3568_vision {

Pipeline::Pipeline()  = default;
Pipeline::~Pipeline() { stop(); }

bool Pipeline::init(const CaptureConfig& cap, const InferenceConfig& inf,
                    const EncodeConfig& enc, const StreamConfig& strm,
                    const DisplayConfig& disp) {
    cap_cfg_ = cap; inf_cfg_ = inf; enc_cfg_ = enc;
    strm_cfg_ = strm; disp_cfg_ = disp;

    capture_  = std::make_unique<V4L2Capture>();
    detector_ = std::make_unique<Detector>();
    encoder_  = std::make_unique<FFmpegEncoder>();
    streamer_ = std::make_unique<RTMPPusher>();
    if (disp_cfg_.enabled) display_ = std::make_unique<Display>(disp_cfg_.window_name);

    return true;
}

bool Pipeline::start() {
    LOG_INFO("Starting Pipeline...");

    if (!capture_->open(cap_cfg_.device, cap_cfg_.width, cap_cfg_.height,
                        cap_cfg_.fps, cap_cfg_.pixel_format, cap_cfg_.buffer_count)) {
        LOG_ERROR("V4L2 capture init failed");
        return false;
    }

    if (inf_cfg_.enabled) {
        if (!detector_->init(inf_cfg_.model_path, inf_cfg_.labels_path,
                             inf_cfg_.conf_threshold, inf_cfg_.nms_threshold,
                             inf_cfg_.npu_core)) {
            LOG_WARN("Detector init failed, running without inference");
        }
    }

    if (enc_cfg_.enabled) {
        if (!encoder_->init(cap_cfg_.width, cap_cfg_.height, cap_cfg_.fps,
                            enc_cfg_.bitrate, enc_cfg_.gop_size,
                            enc_cfg_.codec, enc_cfg_.preset)) {
            LOG_ERROR("Encoder init failed");
            LOG_WARN("Continuing without encoder");
        }

        encoder_->set_packet_callback(
            [this](const uint8_t* d, size_t s, int64_t pts, bool kf) {
                if (streamer_ && streamer_->is_connected()) {
                    streamer_->push_video_packet(d, s, pts, kf);
                }
            });
    }

    if (strm_cfg_.enabled) {
        streamer_->init(strm_cfg_.url, cap_cfg_.width, cap_cfg_.height,
                        cap_cfg_.fps, enc_cfg_.bitrate);
    }

    if (!capture_->start()) {
        LOG_ERROR("V4L2 stream start failed");
        return false;
    }

    running_ = true;

    threads_.emplace_back(&Pipeline::capture_loop, this);
    if (inf_cfg_.enabled && detector_->is_initialized())
        threads_.emplace_back(&Pipeline::inference_loop, this);
    if (enc_cfg_.enabled && encoder_->is_initialized())
        threads_.emplace_back(&Pipeline::encode_loop, this);
    if (disp_cfg_.enabled)
        threads_.emplace_back(&Pipeline::display_loop, this);

    LOG_INFO("Pipeline started: %zu threads", threads_.size());
    return true;
}

void Pipeline::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO("Stopping Pipeline...");

    for (auto& t : threads_) { if (t.joinable()) t.join(); }
    threads_.clear();

    capture_->stop();
    if (encoder_->is_initialized()) encoder_->flush();
    display_->close();

    LOG_INFO("Pipeline stopped");
}

void Pipeline::capture_loop() {
    fps_reset();
    LOG_INFO("Capture thread started");

    while (running_) {
        auto frame = capture_->capture();
        if (!frame) {
            // capture() 已通过 epoll 等待，无需额外 sleep
            if (signal_is_shutdown()) break;
            continue;
        }

        fps_tick();
        frame->capture_ts = now();

        if (inf_cfg_.enabled && detector_->is_initialized()) {
            if (!capture_queue_.push(std::move(frame))) perf_record_drop();
        } else {
            if (!infer_queue_.push(std::move(frame))) perf_record_drop();
        }

        if (signal_is_shutdown()) break;
    }
    LOG_INFO("Capture thread stopped");
}

void Pipeline::inference_loop() {
    fps_reset();
    LOG_INFO("Inference thread started");

    while (running_) {
        std::shared_ptr<FrameBuffer> frame;
        if (!capture_queue_.pop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (signal_is_shutdown()) break;
            continue;
        }

        auto inf_start = now();

        cv::Mat bgr(frame->height, frame->width, CV_8UC3);
        cv::Mat nv12(frame->height + frame->height / 2, frame->width,
                     CV_8UC1, frame->data);
        cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);

        detector_->detect(bgr);
        frame->infer_ts = now();
        frame->bgr = bgr;  // 缓存BGR供display_loop复用

        fps_tick();
        perf_record_infer(inf_start, frame->infer_ts);

        if (!infer_queue_.push(std::move(frame))) perf_record_drop();
        if (signal_is_shutdown()) break;
    }
    LOG_INFO("Inference thread stopped");
}

void Pipeline::encode_loop() {
    fps_reset();
    LOG_INFO("Encode thread started");

    while (running_) {
        std::shared_ptr<FrameBuffer> frame;
        if (!infer_queue_.pop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (signal_is_shutdown()) break;
            continue;
        }

        encoder_->encode(frame->data, frame->sequence);
        fps_tick();

        if (signal_is_shutdown()) break;
    }
    LOG_INFO("Encode thread stopped");
}

void Pipeline::display_loop() {
    fps_reset();
    LOG_INFO("Display thread started");

    while (running_) {
        std::shared_ptr<FrameBuffer> frame;
        if (!infer_queue_.pop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (signal_is_shutdown()) break;
            continue;
        }

        // 复用inference_loop缓存的BGR；无推理时自行转换
        if (!frame->bgr.empty()) {
            display_->show(frame->bgr, disp_cfg_.show_fps, fps_get());
        } else {
            cv::Mat bgr(frame->height, frame->width, CV_8UC3);
            cv::Mat nv12(frame->height + frame->height / 2, frame->width,
                         CV_8UC1, frame->data);
            cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
            display_->show(bgr, disp_cfg_.show_fps, fps_get());
        }
        fps_tick();

        if (signal_is_shutdown()) break;
    }
    LOG_INFO("Display thread stopped");
}

} // namespace rk3568_vision
