#include "rtmp.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

struct rtmp_s {
    AVFormatContext* ctx;
    AVStream*        vs;
    char             url[256];
    uint32_t         width, height, fps;
    int              connected;
};

rtmp_t* rtmp_open(const char* url, uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate) {
    rtmp_t* r = calloc(1, sizeof(rtmp_t));
    if (!r) return NULL;
    strncpy(r->url, url, 255);
    r->width = width; r->height = height; r->fps = fps;

    int ret = avformat_alloc_output_context2(&r->ctx, NULL, "flv", url);
    if (ret < 0 || !r->ctx) {
        LOG_ERROR("rtmp: alloc output context failed");
        free(r); return NULL;
    }

    r->vs = avformat_new_stream(r->ctx, NULL);
    r->vs->id = r->ctx->nb_streams - 1;
    r->vs->time_base = (AVRational){1, (int)fps};
    r->vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    r->vs->codecpar->codec_id   = AV_CODEC_ID_H264;
    r->vs->codecpar->width      = width;
    r->vs->codecpar->height     = height;

    ret = avio_open(&r->ctx->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0) {
        LOG_ERROR("rtmp: avio_open failed for %s", url);
        avformat_free_context(r->ctx);
        free(r); return NULL;
    }

    ret = avformat_write_header(r->ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("rtmp: write header failed");
        avio_closep(&r->ctx->pb);
        avformat_free_context(r->ctx);
        free(r); return NULL;
    }

    r->connected = 1;
    LOG_INFO("rtmp connected: %s", url);
    return r;
}

void rtmp_close(rtmp_t* r) {
    if (!r) return;
    r->connected = 0;
    if (r->ctx) {
        if (r->ctx->pb) {
            av_write_trailer(r->ctx);
            avio_closep(&r->ctx->pb);
        }
        avformat_free_context(r->ctx);
    }
    free(r);
}

int rtmp_push_video(rtmp_t* r, const uint8_t* data, size_t size,
                     int64_t pts, int keyframe) {
    if (!r || !r->connected || !r->ctx || !r->vs) return -1;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return -1;
    pkt->data         = (uint8_t*)data;
    pkt->size         = (int)size;
    pkt->stream_index = r->vs->index;
    pkt->pts = pts;
    pkt->dts = pts;
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(r->ctx, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        LOG_ERROR("rtmp: write frame failed");
        r->connected = 0;
        return -1;
    }
    return 0;
}

int rtmp_is_connected(rtmp_t* r) { return r && r->connected; }
