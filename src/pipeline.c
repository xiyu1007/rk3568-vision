#include "pipeline.h"
#include "v4l2.h"
#include "encoder.h"
#include "rtmp.h"
#include "bridge.h"
#include "logger.h"
#include "fps.h"
#include "sig.h"
#include "perf.h"
#include "ringbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define QUEUE_CAP 8

struct pipeline_s {
    app_cfg_t  cfg;
    v4l2_cap_t*   v4l2;
    encoder_t*    enc;
    rtmp_t*       rtmp;
    void*         detector;
    void*         display;

    void*    frames[PIPELINE_FRAME_POOL];
    int      pool_n;

    void*    cap_q_buf[QUEUE_CAP + 1];
    ringbuf_t cap_q;

    void*    inf_q_buf[QUEUE_CAP + 1];
    ringbuf_t inf_q;

    void*    disp_q_buf[QUEUE_CAP + 1];
    ringbuf_t disp_q;

    pthread_t   th_cap, th_inf, th_enc, th_disp;
    int         running;
};

static void frame_free(frame_t* f) {
    if (!f) return;
    int rc = __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
    if (rc > 0) return;  /* still referenced */
    free(f->data);
    free(f->bgr_data);
    free(f);
}

static void frame_ref(frame_t* f) {
    if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
}

pipeline_t* pipeline_create(const app_cfg_t* cfg) {
    pipeline_t* p = calloc(1, sizeof(pipeline_t));
    if (!p) return NULL;
    p->cfg = *cfg;

    ringbuf_init(&p->cap_q,  (void**)p->cap_q_buf,  QUEUE_CAP);
    ringbuf_init(&p->inf_q,  (void**)p->inf_q_buf,  QUEUE_CAP);
    ringbuf_init(&p->disp_q, (void**)p->disp_q_buf, QUEUE_CAP);

    p->v4l2 = v4l2_open(cfg->cap.device, cfg->cap.width, cfg->cap.height,
                         cfg->cap.fps, cfg->cap.pixfmt, cfg->cap.buf_count);
    if (!p->v4l2) { LOG_ERROR("v4l2_open failed"); goto fail; }

    if (cfg->inf.enabled) {
        p->detector = bridge_detector_create();
        if (!p->detector || !bridge_detector_init(p->detector,
                cfg->inf.model_path, cfg->inf.labels_path,
                cfg->inf.conf_thresh, cfg->inf.nms_thresh, cfg->inf.npu_core)) {
            LOG_WARN("detector init failed, disabling inference");
            p->cfg.inf.enabled = 0;
        }
    }

    if (cfg->enc.enabled) {
        p->enc = encoder_open(cfg->cap.width, cfg->cap.height, cfg->cap.fps,
                              cfg->enc.bitrate, cfg->enc.gop_size);
        if (!p->enc) { LOG_WARN("encoder init failed, disabling encode"); p->cfg.enc.enabled = 0; }
    }

    if (cfg->strm.enabled && p->enc) {
        p->rtmp = rtmp_open(cfg->strm.url, cfg->cap.width, cfg->cap.height,
                            cfg->cap.fps, cfg->enc.bitrate);
        if (!p->rtmp) LOG_WARN("rtmp connect failed");
    }

    if (cfg->disp.enabled) {
        p->display = bridge_display_create(cfg->disp.window_name);
        if (!p->display) { LOG_WARN("display init failed, disabling display"); p->cfg.disp.enabled = 0; }
    }

    return p;

fail:
    pipeline_stop(p);
    return NULL;
}

static void* capture_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("capture thread started");

    while (p->running) {
        frame_t* f = v4l2_capture(p->v4l2);
        if (!f) {
            if (sig_shutdown()) break;
            continue;
        }
        fps_tick();
        f->cap_ts = timestamp_now();

        int use_inf = p->cfg.inf.enabled && p->detector;
        if (use_inf) {
            if (!ringbuf_push(&p->cap_q, f)) {
                frame_free(f);
                perf_record_drop();
            }
        } else {
            if (!ringbuf_push(&p->inf_q, f)) {
                frame_free(f);
                perf_record_drop();
                continue;
            }
            if (p->cfg.disp.enabled) {
                frame_ref(f);
                if (!ringbuf_push(&p->disp_q, f)) {
                    frame_free(f);
                    perf_record_drop();
                }
            }
        }
        if (sig_shutdown()) break;
    }
    LOG_INFO("capture thread stopped");
    return NULL;
}

static void nv12_to_bgr(frame_t* f) {
    if (f->bgr_valid) return;
    int w = f->width, h = f->height;
    int bgr_size = w * h * 3;
    if (!f->bgr_data) f->bgr_data = malloc(bgr_size);
    if (!f->bgr_data) return;

    uint8_t* y  = f->data;
    uint8_t* uv = f->data + w * h;

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            int yi = i * w + j;
            int uvi = (i / 2) * w + (j & ~1);
            int Y = y[yi];
            int U = uv[uvi] - 128;
            int V = uv[uvi + 1] - 128;

            int r = Y + ((359 * V) >> 8);
            int g = Y - ((88 * U + 183 * V) >> 8);
            int b = Y + ((454 * U) >> 8);

            int bi = (i * w + j) * 3;
            f->bgr_data[bi + 0] = b < 0 ? 0 : (b > 255 ? 255 : b);
            f->bgr_data[bi + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
            f->bgr_data[bi + 2] = r < 0 ? 0 : (r > 255 ? 255 : r);
        }
    }
    f->bgr_valid = 1;
}

static void* inference_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("inference thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->cap_q);
        if (!f) { usleep(1000); if (sig_shutdown()) break; continue; }

        int64_t t0 = (int64_t)timestamp_now();

        nv12_to_bgr(f);

        bridge_detector_detect(p->detector, f->bgr_data,
                                f->width, f->height, &f->detect);
        f->inf_ts = timestamp_now();
        perf_record_infer((int64_t)(f->inf_ts - t0));

        fps_tick();

        ringbuf_push(&p->inf_q, f);
        if (p->cfg.disp.enabled) {
            frame_ref(f);
            if (!ringbuf_push(&p->disp_q, f))
                frame_free(f);
        }

        if (sig_shutdown()) break;
    }
    LOG_INFO("inference thread stopped");
    return NULL;
}

static void rtmp_callback(void* userdata, const uint8_t* data,
                           size_t size, int64_t pts, int keyframe) {
    rtmp_t* r = (rtmp_t*)userdata;
    if (r && rtmp_is_connected(r))
        rtmp_push_video(r, data, size, pts, keyframe);
}

static void* encode_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("encode thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->inf_q);
        if (!f) { usleep(5000); if (sig_shutdown()) break; continue; }

        encoder_encode(p->enc, f->data, (int64_t)f->seq);
        fps_tick();
        frame_free(f);
        if (sig_shutdown()) break;
    }
    LOG_INFO("encode thread stopped");
    return NULL;
}

static void* display_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("display thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->disp_q);
        if (!f) { usleep(1000); if (sig_shutdown()) break; continue; }

        if (!f->bgr_valid)
            nv12_to_bgr(f);

        if (f->bgr_valid) {
            bridge_display_show(p->display, f->bgr_data,
                                 f->width, f->height,
                                 p->cfg.disp.show_fps, fps_get(), &f->detect);
        }
        fps_tick();
        frame_free(f);

        if (sig_shutdown()) break;
    }
    LOG_INFO("display thread stopped");
    return NULL;
}

int pipeline_start(pipeline_t* p) {
    if (!p) return 0;

    if (v4l2_start(p->v4l2) < 0) { LOG_ERROR("v4l2_start failed"); return 0; }

    if (p->enc && p->rtmp)
        encoder_set_callback(p->enc, rtmp_callback, p->rtmp);

    p->running = 1;

    pthread_create(&p->th_cap,  NULL, capture_thread,    p);
    if (p->cfg.inf.enabled && p->detector)
        pthread_create(&p->th_inf,  NULL, inference_thread,   p);
    if (p->cfg.enc.enabled && p->enc)
        pthread_create(&p->th_enc,  NULL, encode_thread,      p);
    if (p->cfg.disp.enabled && p->display)
        pthread_create(&p->th_disp, NULL, display_thread,     p);

    LOG_INFO("pipeline started");
    return 1;
}

void pipeline_stop(pipeline_t* p) {
    if (!p) return;
    if (!p->running) { free(p); return; }
    p->running = 0;

    pthread_join(p->th_cap,  NULL);
    if (p->cfg.inf.enabled && p->detector)  pthread_join(p->th_inf,  NULL);
    if (p->cfg.enc.enabled && p->enc)       pthread_join(p->th_enc,  NULL);
    if (p->cfg.disp.enabled && p->display)  pthread_join(p->th_disp, NULL);

    if (p->v4l2) { v4l2_stop(p->v4l2); v4l2_close(p->v4l2); p->v4l2 = NULL; }
    if (p->enc)  { encoder_flush(p->enc); encoder_close(p->enc); p->enc = NULL; }
    if (p->rtmp) { rtmp_close(p->rtmp); p->rtmp = NULL; }
    if (p->detector) { bridge_detector_destroy(p->detector); p->detector = NULL; }
    if (p->display)  { bridge_display_destroy(p->display); p->display = NULL; }

    frame_t* f;
    while ((f = ringbuf_pop(&p->cap_q)))  frame_free(f);
    while ((f = ringbuf_pop(&p->inf_q)))  frame_free(f);
    while ((f = ringbuf_pop(&p->disp_q))) frame_free(f);

    LOG_INFO("pipeline stopped");
    free(p);
}

int pipeline_running(pipeline_t* p) { return p && p->running; }
