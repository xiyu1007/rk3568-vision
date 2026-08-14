// ============================================================================
// encoder.cpp — H.264 编码器实现
// ============================================================================

#include "encoder.hpp"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include "logger.hpp"

namespace vision {

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
H264Encoder::~H264Encoder() {
    close();
}

// ---------------------------------------------------------------------------
// open：初始化编码器
// ---------------------------------------------------------------------------
bool H264Encoder::open(const EncodeConfig& cfg, uint32_t width, uint32_t height,
                       uint32_t fps) {
    width_  = width;
    height_ = height;

    // 1. 选择编码器：硬编优先，失败回退软编。
    if (cfg.hardware) {
        codec_ = avcodec_find_encoder_by_name("h264_rkmpp");
        if (codec_) {
            hardware_ = true;
            LOG_INFO("encoder: using h264_rkmpp (hardware)");
        } else {
            LOG_WARN("encoder: h264_rkmpp not found, fallback to libx264");
        }
    }
    if (!codec_) {
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        hardware_ = false;
        if (!codec_) {
            LOG_ERROR("encoder: no H.264 encoder available");
            return false;
        }
        LOG_INFO("encoder: using libx264 (software)");
    }

    // 2. 分配并配置编码器上下文。
    ctx_ = avcodec_alloc_context3(codec_);
    ctx_->width     = width;
    ctx_->height    = height;
    ctx_->time_base = AVRational{1, static_cast<int>(fps)};
    ctx_->framerate = AVRational{static_cast<int>(fps), 1};
    ctx_->bit_rate  = cfg.bitrate;
    ctx_->gop_size  = cfg.gop_size;
    ctx_->max_b_frames = 0;                          // 零 B 帧 → 低延迟
    ctx_->pix_fmt = hardware_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

    // 软编附加参数（preset/profile/zerolatency 只对 libx264 有意义）。
    if (!hardware_) {
        av_opt_set(ctx_->priv_data, "preset",      cfg.preset.c_str(), 0);
        av_opt_set(ctx_->priv_data, "profile",     cfg.profile.c_str(), 0);
        av_opt_set(ctx_->priv_data, "tune",        "zerolatency", 0);
    }

    // 3. 打开编码器。
    if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
        LOG_ERROR("encoder: avcodec_open2 failed");
        close();
        return false;
    }

    // 4. 分配目标帧（软编分配缓冲，硬编零拷贝直接引用 NV12）。
    dst_frame_ = av_frame_alloc();
    dst_frame_->format = ctx_->pix_fmt;
    dst_frame_->width  = width;
    dst_frame_->height = height;
    if (!hardware_) {
        av_frame_get_buffer(dst_frame_, 32);
        // 初始化 swscale：NV12 → YUV420P。
        sws_ = sws_getContext(width, height, AV_PIX_FMT_NV12,
                              width, height, AV_PIX_FMT_YUV420P,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }

    return true;
}

// ---------------------------------------------------------------------------
// encode：编码一帧
// ---------------------------------------------------------------------------
bool H264Encoder::encode(const FramePtr& frame,
                         const std::function<void(const PacketPtr&)>& on_packet) {
    if (!ctx_ || !frame) return false;

    if (hardware_) {
        // 硬编：NV12 直接引用（零拷贝）。
        dst_frame_->data[0]     = frame->nv12.data();
        dst_frame_->data[1]     = frame->nv12.data() +
                                  static_cast<size_t>(width_) * height_;
        dst_frame_->linesize[0] = width_;
        dst_frame_->linesize[1] = width_;
    } else {
        // 软编：确保目标帧缓冲独占（avcodec_send_frame 内部会引用缓冲，
        // 若直接覆盖会与编码器内部引用冲突）。
        av_frame_make_writable(dst_frame_);
        // NV12 → YUV420P（swscale 平面重排）。
        const uint8_t* src_data[2] = {
            frame->nv12.data(),
            frame->nv12.data() + static_cast<size_t>(width_) * height_,
        };
        const int src_linesize[2] = { static_cast<int>(width_),
                                      static_cast<int>(width_) };
        sws_scale(sws_, src_data, src_linesize, 0, height_,
                  dst_frame_->data, dst_frame_->linesize);
    }

    dst_frame_->pts = pts_++;   // 时间戳以 1/fps 为单位递增

    // 送入编码器。
    int ret = avcodec_send_frame(ctx_, dst_frame_);
    if (ret < 0) {
        LOG_WARN("encoder: send_frame failed (%d)", ret);
        return false;
    }

    // 取出所有已完成编码的包。
    while (true) {
        PacketPtr pkt = makePacket();
        ret = avcodec_receive_packet(ctx_, pkt.get());
        if (ret == 0) {
            on_packet(pkt);            // 交给下游（推流/录制）
        } else {
            break;                     // EAGAIN（需要更多帧）或 EOF
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush：冲刷编码器缓冲
// ---------------------------------------------------------------------------
void H264Encoder::flush(const std::function<void(const PacketPtr&)>& on_packet) {
    if (!ctx_) return;
    avcodec_send_frame(ctx_, nullptr);   // 发送 NULL 表示结束
    while (true) {
        PacketPtr pkt = makePacket();
        int ret = avcodec_receive_packet(ctx_, pkt.get());
        if (ret == 0) {
            on_packet(pkt);
        } else {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// close：释放资源
// ---------------------------------------------------------------------------
void H264Encoder::close() {
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (dst_frame_) {
        av_frame_free(&dst_frame_);
    }
    if (ctx_) {
        avcodec_free_context(&ctx_);
    }
    codec_ = nullptr;
}

// ---------------------------------------------------------------------------
// extradata 访问器
// ---------------------------------------------------------------------------
const uint8_t* H264Encoder::extradata() const {
    return ctx_ ? ctx_->extradata : nullptr;
}

int H264Encoder::extradata_size() const {
    return ctx_ ? ctx_->extradata_size : 0;
}

} // namespace vision
