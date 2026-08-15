// ============================================================================
// muxer.cpp — 视频封装输出实现（FLV/RTMP、MP4）
// ============================================================================

#include "vision/muxer.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include "vision/logger.hpp"

namespace vision {

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
Muxer::~Muxer() {
    Close();
}

// ---------------------------------------------------------------------------
// Open：分配输出上下文、创建流、写头
// ---------------------------------------------------------------------------
bool Muxer::Open(const std::string& format, const std::string& url,
                 uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                 const uint8_t* extradata, int extradata_size, bool add_silent_audio) {
    fps_ = fps;

    // 1. 分配输出上下文。
    int ret = avformat_alloc_output_context2(&context_, nullptr, format.c_str(), url.c_str());
    if (ret < 0 || context_ == nullptr) {
        Logger::instance().error("muxer: alloc failed (%s)", url.c_str());
        return false;
    }

    // 2. 创建视频流。
    video_stream_ = avformat_new_stream(context_, nullptr);
    if (video_stream_ == nullptr) {
        Logger::instance().error("muxer: new video stream failed");
        Close();
        return false;
    }
    video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream_->codecpar->codec_id   = AV_CODEC_ID_H264;
    video_stream_->codecpar->width      = static_cast<int>(width);
    video_stream_->codecpar->height     = static_cast<int>(height);
    video_stream_->codecpar->bit_rate   = static_cast<int64_t>(bitrate);
    video_stream_->time_base            = AVRational{1, static_cast<int>(fps)};
    video_stream_->avg_frame_rate       = AVRational{static_cast<int>(fps), 1};
    video_stream_->r_frame_rate         = AVRational{static_cast<int>(fps), 1};

    // 3. 复制 SPS/PPS（AVCC 格式）。
    if (extradata != nullptr && extradata_size > 0) {
        video_stream_->codecpar->extradata =
            static_cast<uint8_t*>(av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(video_stream_->codecpar->extradata, extradata, extradata_size);
        video_stream_->codecpar->extradata_size = extradata_size;
    }

    // 4. FLV 推流补静音 AAC 音轨（mediamtx 需音轨才转发视频）。
    if (add_silent_audio) {
        InitializeSilentAudio();
    }

    // 5. 打开输出 + 写头。
    if (avio_open(&context_->pb, url.c_str(), AVIO_FLAG_WRITE) < 0) {
        Logger::instance().error("muxer: avio_open failed (%s)", url.c_str());
        Close();
        return false;
    }
    if (avformat_write_header(context_, nullptr) < 0) {
        Logger::instance().error("muxer: write header failed");
        Close();
        return false;
    }
    Logger::instance().info("muxer: opened %s -> %s", format.c_str(), url.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Push：写入一个 H264 包
// ---------------------------------------------------------------------------
bool Muxer::Push(const PacketPtr& packet) {
    if (context_ == nullptr || !packet) {
        return false;
    }
    AVPacket* temp = av_packet_alloc();
    av_packet_ref(temp, packet.get());
    temp->stream_index = video_stream_->index;
    av_packet_rescale_ts(temp, AVRational{1, static_cast<int>(fps_)},
                         video_stream_->time_base);
    const int ret = av_interleaved_write_frame(context_, temp);
    av_packet_free(&temp);
    if (ret < 0) {
        Logger::instance().warn("muxer: write failed (%d)", ret);
        return false;
    }
    WriteSilentAudio(packet->pts);   // 补一路静音音轨（时间戳对齐）
    return true;
}

// ---------------------------------------------------------------------------
// InitializeSilentAudio：添加一路静音 AAC 音轨
// ---------------------------------------------------------------------------
// 本项目摄像头无麦克风、不需要真实声音，此音轨仅用于满足 mediamtx 对 RTMP 流
// 「需含音频轨才转发视频」的要求。不调用板端原生 AAC 编码器（版本错配会段错误），
// 改用 ffmpeg CLI 预生成的静音 AAC-LC 帧硬编码字节。
void Muxer::InitializeSilentAudio() {
    audio_stream_ = avformat_new_stream(context_, nullptr);
    audio_stream_->codecpar->codec_type     = AVMEDIA_TYPE_AUDIO;
    audio_stream_->codecpar->codec_id       = AV_CODEC_ID_AAC;
    audio_stream_->codecpar->sample_rate    = 44100;
    audio_stream_->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
    audio_stream_->codecpar->channels       = 1;
    audio_stream_->time_base                = AVRational{1, 44100};

    // AudioSpecificConfig：AAC-LC / 44100 Hz / 单声道（2 字节）。
    static const uint8_t kAudioSpecificConfig[2] = {0x12, 0x08};
    audio_stream_->codecpar->extradata = static_cast<uint8_t*>(
        av_mallocz(sizeof(kAudioSpecificConfig) + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(audio_stream_->codecpar->extradata,
                kAudioSpecificConfig, sizeof(kAudioSpecificConfig));
    audio_stream_->codecpar->extradata_size = sizeof(kAudioSpecificConfig);

    Logger::instance().info("muxer: silent AAC track added");
}

// ---------------------------------------------------------------------------
// WriteSilentAudio：写一路静音音频包（时间戳跟随视频对齐）
// ---------------------------------------------------------------------------
// 静音帧字节硬编码（ffmpeg CLI 生成），无需编码器，也不缓存到成员避免堆分配。
void Muxer::WriteSilentAudio(int64_t video_pts) {
    if (audio_stream_ == nullptr) {
        return;
    }
    static const uint8_t kSilentAacFrame[4] = {0x01, 0x18, 0x20, 0x07};

    // 音频时间基 1/44100，视频时间基 1/fps_。按视频时间戳换算目标音频采样数，
    // 每次写 1024 样本直到追上。否则音频时间戳严重滞后，mediamtx 转发时会把
    // 整条流当成异常而只发头不发帧。
    const int64_t target = video_pts * 44100 / static_cast<int64_t>(fps_);
    do {
        AVPacket* packet = av_packet_alloc();
        av_new_packet(packet, sizeof(kSilentAacFrame));
        std::memcpy(packet->data, kSilentAacFrame, sizeof(kSilentAacFrame));
        packet->stream_index = audio_stream_->index;
        packet->pts = audio_pts_;
        packet->dts = audio_pts_;
        audio_pts_ += 1024;   // AAC-LC 每帧 1024 样本
        av_interleaved_write_frame(context_, packet);
        av_packet_free(&packet);
    } while (audio_pts_ < target);
}

// ---------------------------------------------------------------------------
// Close：写 trailer 并释放
// ---------------------------------------------------------------------------
void Muxer::Close() {
    if (context_ != nullptr) {
        if (context_->pb != nullptr) {
            av_write_trailer(context_);
            avio_closep(&context_->pb);
        }
        avformat_free_context(context_);
        context_ = nullptr;
        video_stream_ = nullptr;
        audio_stream_ = nullptr;
    }
}

} // namespace vision
