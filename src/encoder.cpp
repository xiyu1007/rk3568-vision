// ============================================================================
// encoder.cpp — H.264 编码器 + 封装输出器实现
// ============================================================================

#include "encoder.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "logger.hpp"

namespace vision {

// ===========================================================================
//  H264Encoder
// ===========================================================================
H264Encoder::~H264Encoder() { close(); }

bool H264Encoder::open(const EncodeConfig& cfg, uint32_t width, uint32_t height,
                       uint32_t fps) {
    width_ = width; height_ = height;

    // 1. 选编码器：硬编优先，失败回退软编。
    if (cfg.hardware) {
        codec_ = avcodec_find_encoder_by_name("h264_rkmpp");
        if (codec_) { hardware_ = true; LOG_INFO("encoder: h264_rkmpp (hardware)"); }
        else        LOG_WARN("encoder: h264_rkmpp not found, fallback libx264");
    }
    if (!codec_) {
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        hardware_ = false;
        if (!codec_) { LOG_ERROR("encoder: no H.264 encoder"); return false; }
        LOG_INFO("encoder: libx264 (software)");
    }

    // 2. 配置上下文。
    ctx_ = avcodec_alloc_context3(codec_);
    ctx_->width     = width;
    ctx_->height    = height;
    ctx_->time_base = AVRational{1, (int)fps};
    ctx_->framerate = AVRational{(int)fps, 1};
    ctx_->bit_rate  = cfg.bitrate;
    ctx_->gop_size  = cfg.gop_size;
    ctx_->max_b_frames = 0;                 // 零 B 帧，降延迟
    ctx_->pix_fmt = hardware_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    // 关键：让编码器把 SPS/PPS 写入 extradata（AVCC 格式），而非塞进码流(Annex-B)。
    // 否则 FLV/RTMP 推流拿不到 codec 配置，nginx 会在首包后断开连接。
    ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!hardware_) {
        av_opt_set(ctx_->priv_data, "preset",  cfg.preset.c_str(), 0);
        av_opt_set(ctx_->priv_data, "profile", cfg.profile.c_str(), 0);
        av_opt_set(ctx_->priv_data, "tune",    "zerolatency", 0);
    }
    if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
        LOG_ERROR("encoder: avcodec_open2 failed"); close(); return false;
    }

    // 3. 分配目标帧。
    dst_frame_ = av_frame_alloc();
    dst_frame_->format = ctx_->pix_fmt;
    dst_frame_->width  = width;
    dst_frame_->height = height;
    if (!hardware_) {
        av_frame_get_buffer(dst_frame_, 32);
        sws_ = sws_getContext(width, height, AV_PIX_FMT_NV12,
                              width, height, AV_PIX_FMT_YUV420P,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }
    return true;
}

bool H264Encoder::encode(const FramePtr& frame,
                         const std::function<void(const PacketPtr&)>& on_packet) {
    if (!ctx_ || !frame) return false;

    if (hardware_) {
        // 硬编：NV12 零拷贝直通。
        dst_frame_->data[0]     = frame->nv12.data();
        dst_frame_->data[1]     = frame->nv12.data() + (size_t)width_ * height_;
        dst_frame_->linesize[0] = width_;
        dst_frame_->linesize[1] = width_;
    } else {
        // 软编：NV12 → YUV420P。
        av_frame_make_writable(dst_frame_);
        const uint8_t* src[2] = { frame->nv12.data(),
                                  frame->nv12.data() + (size_t)width_ * height_ };
        const int ls[2] = { (int)width_, (int)width_ };
        sws_scale(sws_, src, ls, 0, height_, dst_frame_->data, dst_frame_->linesize);
    }

    dst_frame_->pts = pts_++;
    if (avcodec_send_frame(ctx_, dst_frame_) < 0) return false;

    while (true) {
        PacketPtr pkt = makePacket();
        if (avcodec_receive_packet(ctx_, pkt.get()) == 0) on_packet(pkt);
        else break;
    }
    return true;
}

void H264Encoder::flush(const std::function<void(const PacketPtr&)>& on_packet) {
    if (!ctx_) return;
    avcodec_send_frame(ctx_, nullptr);
    while (true) {
        PacketPtr pkt = makePacket();
        if (avcodec_receive_packet(ctx_, pkt.get()) == 0) on_packet(pkt);
        else break;
    }
}

void H264Encoder::close() {
    if (sws_)        { sws_freeContext(sws_); sws_ = nullptr; }
    if (dst_frame_)  { av_frame_free(&dst_frame_); }
    if (ctx_)        { avcodec_free_context(&ctx_); }
    codec_ = nullptr;
}

const uint8_t* H264Encoder::extradata() const { return ctx_ ? ctx_->extradata : nullptr; }
int H264Encoder::extradata_size() const { return ctx_ ? ctx_->extradata_size : 0; }

// ===========================================================================
//  Muxer
// ===========================================================================
Muxer::~Muxer() { close(); }

bool Muxer::open(const std::string& format, const std::string& url,
                 uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                 const uint8_t* extradata, int extradata_size) {
    fps_ = fps;

    int ret = avformat_alloc_output_context2(&ctx_, nullptr, format.c_str(), url.c_str());
    if (ret < 0 || !ctx_) { LOG_ERROR("muxer: alloc failed (%s)", url.c_str()); return false; }

    vs_ = avformat_new_stream(ctx_, nullptr);
    if (!vs_) { LOG_ERROR("muxer: new stream failed"); close(); return false; }
    vs_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs_->codecpar->codec_id   = AV_CODEC_ID_H264;
    vs_->codecpar->width      = width;
    vs_->codecpar->height     = height;
    vs_->codecpar->bit_rate   = bitrate;
    vs_->time_base            = AVRational{1, (int)fps};
    vs_->avg_frame_rate       = AVRational{(int)fps, 1};
    vs_->r_frame_rate         = AVRational{(int)fps, 1};

    if (extradata && extradata_size > 0) {
        vs_->codecpar->extradata =
            (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(vs_->codecpar->extradata, extradata, extradata_size);
        vs_->codecpar->extradata_size = extradata_size;
    }

    // mediamtx v1.9.3 对「纯视频、无音频」的 RTMP 流，会正确返回 FLV 头与 H264
    // 序列头，但不向拉流端转发任何视频包（ffmpeg CLI `-c:v libx264 -an` 推流同样
    // 复现：拉流端 0 帧被解封装）；补一路音轨后立即正常。故为 FLV 推流补静音 AAC。
    if (format == "flv") {
        initSilentAudio();
    }

    if (avio_open(&ctx_->pb, url.c_str(), AVIO_FLAG_WRITE) < 0) {
        LOG_ERROR("muxer: avio_open failed (%s)", url.c_str()); close(); return false;
    }
    if (avformat_write_header(ctx_, nullptr) < 0) {
        LOG_ERROR("muxer: write header failed"); close(); return false;
    }
    LOG_INFO("muxer: opened %s -> %s", format.c_str(), url.c_str());
    return true;
}

bool Muxer::push(const PacketPtr& pkt) {
    if (!ctx_ || !pkt) return false;
    AVPacket* tmp = av_packet_alloc();
    av_packet_ref(tmp, pkt.get());
    tmp->stream_index = vs_->index;
    av_packet_rescale_ts(tmp, AVRational{1, (int)fps_}, vs_->time_base);
    int ret = av_interleaved_write_frame(ctx_, tmp);
    av_packet_free(&tmp);
    if (ret < 0) { LOG_WARN("muxer: write failed (%d)", ret); return false; }
    writeSilentAudio(pkt->pts);   // 补一路静音音轨（mediamtx 需音轨才转发视频）
    return true;
}

// 为 FLV/RTMP 补一路静音 AAC 音轨。本项目摄像头无麦克风、不需要真实声音，
// 此音轨仅用于满足 mediamtx 对 RTMP 流「需含音频轨才转发视频」的要求（见 open 内注释）。
//
// 不调用板端原生 AAC 编码器（该 FFmpeg 版本错配，avcodec_receive_packet 会返回非法
// 数据指针导致段错误），改用 ffmpeg CLI 预生成的静音 AAC-LC 帧硬编码字节。
void Muxer::initSilentAudio() {
    as_ = avformat_new_stream(ctx_, nullptr);
    as_->codecpar->codec_type     = AVMEDIA_TYPE_AUDIO;
    as_->codecpar->codec_id       = AV_CODEC_ID_AAC;
    as_->codecpar->sample_rate    = 44100;
    as_->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
    as_->codecpar->channels       = 1;
    as_->time_base                = AVRational{1, 44100};

    // AudioSpecificConfig：AAC-LC / 44100 Hz / 单声道（2 字节）。
    static const uint8_t kAudioSpecificConfig[2] = {0x12, 0x08};
    as_->codecpar->extradata =
        (uint8_t*)av_mallocz(sizeof(kAudioSpecificConfig) + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(as_->codecpar->extradata, kAudioSpecificConfig, sizeof(kAudioSpecificConfig));
    as_->codecpar->extradata_size = sizeof(kAudioSpecificConfig);

    LOG_INFO("muxer: silent AAC track added");
}

// 写一路静音音频包，时间戳跟随视频对齐。
// 静音帧字节硬编码（ffmpeg CLI 生成），无需编码器，也不缓存到成员避免堆分配。
void Muxer::writeSilentAudio(int64_t video_pts) {
    if (!as_) return;
    static const uint8_t kSilentAacFrame[4] = {0x01, 0x18, 0x20, 0x07};

    // 音频时间基 1/44100，视频时间基 1/fps_。按视频时间戳换算目标音频采样数，
    // 每次写 1024 样本直到追上。否则音频时间戳严重滞后，mediamtx 转发时会把
    // 整条流当成异常而只发头不发帧。
    int64_t target = video_pts * 44100 / (int64_t)fps_;
    do {
        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, sizeof(kSilentAacFrame));
        std::memcpy(pkt->data, kSilentAacFrame, sizeof(kSilentAacFrame));
        pkt->stream_index = as_->index;
        pkt->pts = audio_pts_;
        pkt->dts = audio_pts_;
        audio_pts_ += 1024;   // AAC-LC 每帧 1024 样本
        av_interleaved_write_frame(ctx_, pkt);
        av_packet_free(&pkt);
    } while (audio_pts_ < target);
}

void Muxer::close() {
    if (ctx_) {
        if (ctx_->pb) { av_write_trailer(ctx_); avio_closep(&ctx_->pb); }
        avformat_free_context(ctx_);
        ctx_ = nullptr; vs_ = nullptr; as_ = nullptr;
    }
}

} // namespace vision
