#include "ffmpeg_encoder.hpp"
#include "logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace rk3568_vision {

FFmpegEncoder::FFmpegEncoder() { packet_ = av_packet_alloc(); }
FFmpegEncoder::~FFmpegEncoder() {
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (frame_)     av_frame_free(&frame_);
    if (packet_)    av_packet_free(&packet_);
    if (sws_)       sws_freeContext(sws_);
}

bool FFmpegEncoder::init(uint32_t w, uint32_t h, uint32_t f,
                         uint32_t br, uint32_t gop,
                         const std::string& /*codec_name*/,
                         const std::string& /*preset*/) {
    width_ = w; height_ = h;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) { LOG_ERROR("libx264 not found"); return false; }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) { LOG_ERROR("alloc context failed"); return false; }

    codec_ctx_->width     = w;
    codec_ctx_->height    = h;
    codec_ctx_->time_base = AVRational{1, (int)f};
    codec_ctx_->pix_fmt   = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate  = br;
    codec_ctx_->gop_size  = gop;

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        LOG_ERROR("avcodec_open2 failed: %s (w=%u h=%u f=%u)", err, w, h, f);
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = codec_ctx_->pix_fmt;
    frame_->width  = w; frame_->height = h;
    av_frame_get_buffer(frame_, 0);

    sws_ = sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_YUV420P,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    initialized_ = true;
    LOG_INFO("Encoder: %s %ux%u@%ufps %ukbps", codec->name, w, h, f, br/1000);
    return true;
}

bool FFmpegEncoder::encode(const uint8_t* nv12, int64_t pts) {
    if (!initialized_ || !codec_ctx_ || !frame_ || !nv12) return false;

    const uint8_t* src[2] = { nv12, nv12 + width_ * height_ };
    int strides[2] = { (int)width_, (int)width_ };
    sws_scale(sws_, src, strides, 0, height_, frame_->data, frame_->linesize);

    if (pts < 0) pts = frame_counter_;
    frame_->pts = pts; frame_counter_++;

    int ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0) return false;

    while (ret >= 0) {
        av_packet_unref(packet_);
        ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;
        if (packet_cb_) packet_cb_(packet_->data, packet_->size, packet_->pts,
                                   (packet_->flags & AV_PKT_FLAG_KEY));
    }
    return true;
}

bool FFmpegEncoder::flush() {
    if (!initialized_) return false;
    avcodec_send_frame(codec_ctx_, nullptr);
    while (true) {
        av_packet_unref(packet_);
        int ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) return false;
        if (packet_cb_) packet_cb_(packet_->data, packet_->size, packet_->pts,
                                   (packet_->flags & AV_PKT_FLAG_KEY));
    }
    return true;
}

} // namespace rk3568_vision
