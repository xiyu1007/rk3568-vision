#include "rtmp_pusher.hpp"
#include "logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace rk3568_vision {

RTMPPusher::RTMPPusher()  = default;
RTMPPusher::~RTMPPusher() { do_disconnect(); }

bool RTMPPusher::init(const std::string& url, uint32_t width, uint32_t height,
                      uint32_t fps, uint32_t bitrate) {
    url_     = url;
    width_   = width;
    height_  = height;
    fps_     = fps;
    bitrate_ = bitrate;

    return do_connect();
}

bool RTMPPusher::do_connect() {
    if (fmt_ctx_) do_disconnect();

    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "flv", url_.c_str());
    if (ret < 0 || !fmt_ctx_) {
        LOG_ERROR("Failed to create RTMP output context: %s", url_.c_str());
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { LOG_ERROR("H.264 encoder not found"); return false; }

    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    video_stream_->id = fmt_ctx_->nb_streams - 1;
    video_stream_->time_base = {1, static_cast<int>(fps_)};
    video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream_->codecpar->codec_id   = AV_CODEC_ID_H264;
    video_stream_->codecpar->width      = width_;
    video_stream_->codecpar->height     = height_;

    ret = avio_open(&fmt_ctx_->pb, url_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        LOG_ERROR("Failed to open RTMP URL: %s", url_.c_str());
        return false;
    }

    AVDictionary* opts = nullptr;
    ret = avformat_write_header(fmt_ctx_, &opts);
    if (ret < 0) {
        LOG_ERROR("Failed to write RTMP header");
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    connected_ = true;
    LOG_INFO("RTMP connected: %s", url_.c_str());
    return true;
}

void RTMPPusher::do_disconnect() {
    connected_ = false;
    if (fmt_ctx_) {
        if (fmt_ctx_->pb) {
            av_write_trailer(fmt_ctx_);
            avio_closep(&fmt_ctx_->pb);
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        video_stream_ = nullptr;
    }
}

bool RTMPPusher::reconnect() {
    do_disconnect();
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (do_connect()) return true;
        LOG_WARN("RTMP reconnect attempt %d/5 failed", i + 1);
    }
    return false;
}

bool RTMPPusher::push_video_packet(const uint8_t* data, size_t size,
                                    int64_t pts, bool keyframe) {
    if (!connected_ || !fmt_ctx_ || !video_stream_) return false;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data         = const_cast<uint8_t*>(data);
    pkt.size         = static_cast<int>(size);
    pkt.stream_index = video_stream_->index;
    pkt.pts          = pts;
    pkt.dts          = pts;
    if (keyframe) pkt.flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(fmt_ctx_, &pkt);
    if (ret < 0) {
        LOG_ERROR("RTMP write frame failed");
        connected_ = false;
        return false;
    }
    return true;
}

bool RTMPPusher::push_audio_packet(const uint8_t* data, size_t size, int64_t pts) {
    (void)data; (void)size; (void)pts;
    return true;
}

} // namespace rk3568_vision
