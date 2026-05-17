#include "encoder.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

struct encoder_s {
    AVCodecContext* ctx;
    AVFrame*        frame;
    AVPacket*       pkt;
    struct SwsContext* sws;
    uint32_t width, height;
    int64_t  frame_n;
    enc_packet_cb cb;
    void*  cb_data;
};

encoder_t* encoder_open(uint32_t w, uint32_t h, uint32_t fps,
                        uint32_t bitrate, uint32_t gop) {
    encoder_t* e = calloc(1, sizeof(encoder_t));
    if (!e) return NULL;
    e->width = w; e->height = h;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) { LOG_ERROR("libx264 not found"); encoder_close(e); return NULL; }

    e->ctx = avcodec_alloc_context3(codec);
    if (!e->ctx) { LOG_ERROR("avcodec_alloc_context3 failed"); encoder_close(e); return NULL; }

    e->ctx->width    = w;
    e->ctx->height   = h;
    e->ctx->time_base = (AVRational){1, (int)fps};
    e->ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    e->ctx->bit_rate = bitrate;
    e->ctx->gop_size = gop;

    int ret = avcodec_open2(e->ctx, codec, NULL);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_ERROR("avcodec_open2 failed: %s", err);
        encoder_close(e); return NULL;
    }

    e->frame = av_frame_alloc();
    e->frame->format = e->ctx->pix_fmt;
    e->frame->width  = w; e->frame->height = h;
    av_frame_get_buffer(e->frame, 0);

    e->sws = sws_getContext(w, h, AV_PIX_FMT_NV12,
                            w, h, AV_PIX_FMT_YUV420P,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL);

    e->pkt = av_packet_alloc();

    LOG_INFO("encoder: libx264 %ux%u@%ufps %ukbps", w, h, fps, bitrate / 1000);
    return e;
}

void encoder_close(encoder_t* e) {
    if (!e) return;
    if (e->ctx)   avcodec_free_context(&e->ctx);
    if (e->frame) av_frame_free(&e->frame);
    if (e->pkt)   av_packet_free(&e->pkt);
    if (e->sws)   sws_freeContext(e->sws);
    free(e);
}

int encoder_encode(encoder_t* e, const uint8_t* nv12, int64_t pts) {
    if (!e || !e->ctx || !nv12) return -1;

    const uint8_t* src[2] = { nv12, nv12 + e->width * e->height };
    int strides[2] = { (int)e->width, (int)e->width };
    sws_scale(e->sws, src, strides, 0, e->height,
              e->frame->data, e->frame->linesize);

    if (pts < 0) pts = e->frame_n;
    e->frame->pts = pts;
    e->frame_n++;

    int ret = avcodec_send_frame(e->ctx, e->frame);
    if (ret < 0) return -1;

    while (ret >= 0) {
        av_packet_unref(e->pkt);
        ret = avcodec_receive_packet(e->ctx, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return -1;
        if (e->cb)
            e->cb(e->cb_data, e->pkt->data, e->pkt->size, e->pkt->pts,
                  (e->pkt->flags & AV_PKT_FLAG_KEY));
    }
    return 0;
}

int encoder_flush(encoder_t* e) {
    if (!e || !e->ctx) return -1;
    avcodec_send_frame(e->ctx, NULL);
    for (;;) {
        av_packet_unref(e->pkt);
        int ret = avcodec_receive_packet(e->ctx, e->pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) return -1;
        if (e->cb)
            e->cb(e->cb_data, e->pkt->data, e->pkt->size, e->pkt->pts,
                  (e->pkt->flags & AV_PKT_FLAG_KEY));
    }
    return 0;
}

void encoder_set_callback(encoder_t* e, enc_packet_cb cb, void* userdata) {
    if (e) { e->cb = cb; e->cb_data = userdata; }
}

int encoder_ready(encoder_t* e) { return e && e->ctx ? 1 : 0; }
