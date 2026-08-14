// ============================================================================
// muxer.cpp — FFmpeg 封装输出器实现
// ============================================================================

#include "muxer.hpp"

#include <cstring>

#include <libavformat/avformat.h>

#include "logger.hpp"

namespace vision {

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
Muxer::~Muxer() {
    close();
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
bool Muxer::open(const std::string& format, const std::string& url,
                 uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                 const uint8_t* extradata, int extradata_size) {
    fps_ = fps;

    // 1. 创建输出上下文（"flv" 自动匹配 RTMP 协议；"mp4" 匹配本地文件）。
    int ret = avformat_alloc_output_context2(&ctx_, nullptr, format.c_str(),
                                             url.c_str());
    if (ret < 0 || !ctx_) {
        LOG_ERROR("muxer: alloc output context failed (%s)", url.c_str());
        return false;
    }

    // 2. 新建视频流并设置 H.264 参数。
    vs_ = avformat_new_stream(ctx_, nullptr);
    if (!vs_) {
        LOG_ERROR("muxer: new stream failed");
        close();
        return false;
    }
    vs_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs_->codecpar->codec_id   = AV_CODEC_ID_H264;
    vs_->codecpar->width      = width;
    vs_->codecpar->height     = height;
    vs_->time_base            = AVRational{1, static_cast<int>(fps)};

    // 3. 拷贝 SPS/PPS（extradata），播放器/解码器据此初始化。
    if (extradata && extradata_size > 0) {
        vs_->codecpar->extradata =
            static_cast<uint8_t*>(av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(vs_->codecpar->extradata, extradata, extradata_size);
        vs_->codecpar->extradata_size = extradata_size;
    }

    // 4. 打开输出（RTMP 建连 或 打开本地文件）。
    ret = avio_open(&ctx_->pb, url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        LOG_ERROR("muxer: avio_open failed (%s)", url.c_str());
        close();
        return false;
    }

    // 5. 写容器头。
    ret = avformat_write_header(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("muxer: write header failed");
        close();
        return false;
    }

    LOG_INFO("muxer: opened %s -> %s", format.c_str(), url.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// push：写入一个编码包
// ---------------------------------------------------------------------------
bool Muxer::push(const PacketPtr& pkt) {
    if (!ctx_ || !pkt) return false;

    // 拷贝一个包引用再写（av_interleaved_write_frame 内部会释放它，
    // 不能直接传 shared_ptr 里的裸指针，否则会被二次释放）。
    AVPacket tmp;
    av_packet_ref(&tmp, pkt.get());
    tmp.stream_index = vs_->index;
    // 时间基换算：编码器时间基 {1,fps} → 流时间基（相同，此处为安全起见仍换算）。
    av_packet_rescale_ts(&tmp, AVRational{1, static_cast<int>(fps_)}, vs_->time_base);

    int ret = av_interleaved_write_frame(ctx_, &tmp);
    if (ret < 0) {
        LOG_WARN("muxer: write frame failed (%d)", ret);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------
void Muxer::close() {
    if (ctx_) {
        if (ctx_->pb) {
            av_write_trailer(ctx_);   // 写容器尾
            avio_closep(&ctx_->pb);   // 关闭网络连接/文件
        }
        avformat_free_context(ctx_);
        ctx_ = nullptr;
        vs_  = nullptr;
    }
}

} // namespace vision
