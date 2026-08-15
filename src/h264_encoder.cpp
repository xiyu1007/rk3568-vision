// ============================================================================
// h264_encoder.cpp — H.264 编码器实现
// ============================================================================

#include "vision/h264_encoder.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "vision/logger.hpp"

namespace vision {

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
H264Encoder::~H264Encoder() {
    Close();
}

// ---------------------------------------------------------------------------
// Open：选择编码器并配置上下文
// ---------------------------------------------------------------------------
bool H264Encoder::Open(const EncodeConfig& config, uint32_t width, uint32_t height,
                       uint32_t fps) {
    width_ = width;
    height_ = height;

    // 1. 选编码器：硬编优先，失败回退软编。
    if (config.hardware) {
        codec_ = avcodec_find_encoder_by_name("h264_rkmpp");
        if (codec_ != nullptr) {
            hardware_ = true;
            Logger::instance().info("encoder: h264_rkmpp (hardware)");
        } else {
            Logger::instance().warn("encoder: h264_rkmpp not found, fallback libx264");
        }
    }
    if (codec_ == nullptr) {
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        hardware_ = false;
        if (codec_ == nullptr) {
            Logger::instance().error("encoder: no H.264 encoder");
            return false;
        }
        Logger::instance().info("encoder: libx264 (software)");
    }

    // 2. 配置上下文。
    codec_context_ = avcodec_alloc_context3(codec_);
    codec_context_->width      = static_cast<int>(width);
    codec_context_->height     = static_cast<int>(height);
    codec_context_->time_base  = AVRational{1, static_cast<int>(fps)};
    codec_context_->framerate  = AVRational{static_cast<int>(fps), 1};
    codec_context_->bit_rate   = static_cast<int64_t>(config.bitrate);
    codec_context_->gop_size   = static_cast<int>(config.gop_size);
    codec_context_->max_b_frames = 0;   // 零 B 帧，降延迟
    codec_context_->pix_fmt = hardware_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    // 关键：让编码器把 SPS/PPS 写入 extradata（AVCC 格式），而非塞进码流(Annex-B)。
    // 否则 FLV/RTMP 推流拿不到 codec 配置，服务器会在首包后断开连接。
    codec_context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!hardware_) {
        av_opt_set(codec_context_->priv_data, "preset",  config.preset.c_str(), 0);
        av_opt_set(codec_context_->priv_data, "profile", config.profile.c_str(), 0);
        av_opt_set(codec_context_->priv_data, "tune",    "zerolatency", 0);
    }
    if (avcodec_open2(codec_context_, codec_, nullptr) < 0) {
        Logger::instance().error("encoder: avcodec_open2 failed");
        Close();
        return false;
    }

    // 3. 分配目标帧。
    dst_frame_ = av_frame_alloc();
    dst_frame_->format = codec_context_->pix_fmt;
    dst_frame_->width  = static_cast<int>(width);
    dst_frame_->height = static_cast<int>(height);
    if (!hardware_) {
        av_frame_get_buffer(dst_frame_, 32);
        sws_context_ = sws_getContext(static_cast<int>(width), static_cast<int>(height),
                                      AV_PIX_FMT_NV12,
                                      static_cast<int>(width), static_cast<int>(height),
                                      AV_PIX_FMT_YUV420P,
                                      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Encode：编码一帧
// ---------------------------------------------------------------------------
bool H264Encoder::Encode(const FramePtr& frame,
                         const std::function<void(const PacketPtr&)>& on_packet) {
    if (codec_context_ == nullptr || !frame) {
        return false;
    }

    if (hardware_) {
        // 硬编：NV12 零拷贝直通。
        dst_frame_->data[0]     = const_cast<uint8_t*>(frame->nv12_data);
        dst_frame_->data[1]     = const_cast<uint8_t*>(frame->nv12_data) +
                                  static_cast<size_t>(frame->nv12_stride) * height_;
        dst_frame_->linesize[0] = static_cast<int>(frame->nv12_stride);
        dst_frame_->linesize[1] = static_cast<int>(frame->nv12_stride);
    } else {
        // 软编：NV12 → YUV420P。
        av_frame_make_writable(dst_frame_);
        const uint8_t* src[2] = {
            frame->nv12_data,
            frame->nv12_data + static_cast<size_t>(frame->nv12_stride) * height_};
        const int src_linesize[2] = {
            static_cast<int>(frame->nv12_stride),
            static_cast<int>(frame->nv12_stride)};
        sws_scale(sws_context_, src, src_linesize, 0, static_cast<int>(height_),
                  dst_frame_->data, dst_frame_->linesize);
    }

    dst_frame_->pts = pts_++;
    if (avcodec_send_frame(codec_context_, dst_frame_) < 0) {
        return false;
    }

    while (true) {
        PacketPtr packet = CreatePacket();
        if (avcodec_receive_packet(codec_context_, packet.get()) == 0) {
            on_packet(packet);
        } else {
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Flush：冲刷编码器残留帧
// ---------------------------------------------------------------------------
void H264Encoder::Flush(const std::function<void(const PacketPtr&)>& on_packet) {
    if (codec_context_ == nullptr) {
        return;
    }
    avcodec_send_frame(codec_context_, nullptr);
    while (true) {
        PacketPtr packet = CreatePacket();
        if (avcodec_receive_packet(codec_context_, packet.get()) == 0) {
            on_packet(packet);
        } else {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Close：释放编码器资源
// ---------------------------------------------------------------------------
void H264Encoder::Close() {
    if (sws_context_ != nullptr) {
        sws_freeContext(sws_context_);
        sws_context_ = nullptr;
    }
    if (dst_frame_ != nullptr) {
        av_frame_free(&dst_frame_);
    }
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }
    codec_ = nullptr;
}

const uint8_t* H264Encoder::GetExtradata() const {
    return codec_context_ != nullptr ? codec_context_->extradata : nullptr;
}

int H264Encoder::GetExtradataSize() const {
    return codec_context_ != nullptr ? codec_context_->extradata_size : 0;
}

} // namespace vision
