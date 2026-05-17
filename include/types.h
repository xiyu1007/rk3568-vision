#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Timestamp (microseconds since boot) ─────────────────────────────── */
typedef uint64_t timestamp_us_t;

static inline timestamp_us_t timestamp_now(void);

/* ── Frame buffer (NV12 data + metadata) ─────────────────────────────── */
#define FRAME_MAX_PLANES 4
#define FRAME_LABEL_MAX  64
#define DETECT_MAX_BOXES 64

/* ── Detection result (must be before frame_t) ───────────────────────── */
typedef struct {
    int   x, y, w, h;
    int   class_id;
    float conf;
    char  label[FRAME_LABEL_MAX];
} detect_box_t;

typedef struct {
    uint32_t     count;
    detect_box_t boxes[DETECT_MAX_BOXES];
} detect_result_t;

typedef struct {
    uint8_t* data;
    size_t   size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t seq;
    timestamp_us_t cap_ts;
    timestamp_us_t inf_ts;
    uint8_t* bgr_data;
    bool     bgr_valid;
    int      refcount;
    detect_result_t detect;
} frame_t;

/* ── Performance counters ────────────────────────────────────────────── */
typedef struct {
    int64_t cap_us;          /* capture latency (latest)                 */
    int64_t inf_us;          /* inference latency (latest)               */
    int64_t enc_us;          /* encode latency (latest)                  */
    int64_t total_frames;
    int64_t dropped_frames;
} perf_t;

/* ── Configuration structures ────────────────────────────────────────── */
typedef struct {
    char     device[64];
    uint32_t width, height, fps;
    char     pixfmt[16];
    uint32_t buf_count;
    int      use_mplane;
} cap_cfg_t;

typedef struct {
    bool     enabled;
    char     model_path[256];
    char     labels_path[256];
    float    conf_thresh;
    float    nms_thresh;
    uint32_t model_w, model_h;
    bool     quantized;
    uint32_t npu_core;
} inf_cfg_t;

typedef struct {
    bool     enabled;
    char     codec[16];
    uint32_t bitrate;
    uint32_t gop_size;
} enc_cfg_t;

typedef struct {
    bool     enabled;
    char     url[256];
    bool     reconnect;
    uint32_t reconnect_delay_ms;
    int32_t  max_reconnect;
} strm_cfg_t;

typedef struct {
    bool     enabled;
    char     window_name[64];
    bool     show_fps;
} disp_cfg_t;

typedef struct {
    bool     enabled;
    uint32_t log_interval_ms;
} mon_cfg_t;

typedef struct {
    cap_cfg_t  cap;
    inf_cfg_t  inf;
    enc_cfg_t  enc;
    strm_cfg_t strm;
    disp_cfg_t disp;
    mon_cfg_t  mon;
} app_cfg_t;

/* ── Configuration loading ───────────────────────────────────────────── */
int  config_load(const char* yaml_path, app_cfg_t* cfg);

/* ── Timestamp implementation ────────────────────────────────────────── */
#if defined(__linux__)
#include <time.h>
static inline timestamp_us_t timestamp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#else
#error "timestamp_now() not implemented for this platform"
#endif

#ifdef __cplusplus
}
#endif

#endif /* TYPES_H */
