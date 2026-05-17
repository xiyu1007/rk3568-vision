#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEY_LEN   128
#define MAX_VAL_LEN   256
#define MAX_ENTRIES   128

typedef struct { char key[MAX_KEY_LEN]; char val[MAX_VAL_LEN]; } kv_t;

static kv_t   g_kv[MAX_ENTRIES];
static size_t g_kv_n = 0;

static void kv_set(const char* key, const char* val) {
    if (g_kv_n >= MAX_ENTRIES) return;
    strncpy(g_kv[g_kv_n].key, key, MAX_KEY_LEN - 1);
    strncpy(g_kv[g_kv_n].val, val, MAX_VAL_LEN - 1);
    g_kv_n++;
}

static const char* kv_get(const char* key, const char* def) {
    for (size_t i = 0; i < g_kv_n; i++)
        if (strcmp(g_kv[i].key, key) == 0) return g_kv[i].val;
    return def;
}

static void kv_clear(void) { g_kv_n = 0; }

/* ── Minimal YAML parser ──────────────────────────────────────────────── */

static void trim_right(char* s) {
    char* end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
}

static void trim_left(char** ps) {
    while (**ps == ' ' || **ps == '\t') (*ps)++;
}

static void strip_comment(char* s) {
    char* c = strchr(s, '#');
    if (c) *c = '\0';
}

static void trim_quotes(char* s) {
    int len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void parse_line(char* line, char* section, int* indent) {
    char* p = line;
    trim_left(&p);
    if (*p == '\0') return;

    int new_indent = (int)(p - line);
    strip_comment(p);
    trim_right(p);
    if (*p == '\0') return;

    int len = (int)strlen(p);
    if (p[len - 1] == ':') {
        p[len - 1] = '\0';
        trim_right(p);
        if (new_indent == 0)
            strncpy(section, p, MAX_KEY_LEN - 1);
        else if (section[0]) {
            char tmp[MAX_KEY_LEN];
            snprintf(tmp, sizeof(tmp), "%s.%s", section, p);
            strncpy(section, tmp, MAX_KEY_LEN - 1);
        } else
            strncpy(section, p, MAX_KEY_LEN - 1);
        *indent = new_indent;
        return;
    }

    char* colon = strchr(p, ':');
    if (!colon) return;
    *colon = '\0';
    char* key = p;
    char* val = colon + 1;
    trim_right(key);
    trim_left(&val);
    trim_quotes(val);

    char full_key[MAX_KEY_LEN];
    if (section[0])
        snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
    else
        strncpy(full_key, key, sizeof(full_key) - 1);

    kv_set(full_key, val);
}

int config_load(const char* yaml_path, app_cfg_t* cfg) {
    kv_clear();

    FILE* f = fopen(yaml_path, "r");
    if (!f) {
        LOG_WARN("config file not found: %s, using defaults", yaml_path);
        /* fall through to load defaults via kv_get */
    } else {
        char section[MAX_KEY_LEN] = {0};
        int  indent = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            parse_line(line, section, &indent);
        }
        fclose(f);
        LOG_INFO("config loaded: %s (%zu entries)", yaml_path, g_kv_n);
    }

    memset(cfg, 0, sizeof(*cfg));

    /* capture */
    strncpy(cfg->cap.device, kv_get("capture.device", "/dev/video0"), 63);
    cfg->cap.width   = (uint32_t)atoi(kv_get("capture.width",  "1920"));
    cfg->cap.height  = (uint32_t)atoi(kv_get("capture.height", "1080"));
    cfg->cap.fps     = (uint32_t)atoi(kv_get("capture.fps",    "30"));
    strncpy(cfg->cap.pixfmt, kv_get("capture.pixel_format", "NV12"), 15);
    cfg->cap.buf_count = (uint32_t)atoi(kv_get("capture.buffer_count", "6"));
    {
        const char* bt = kv_get("capture.buffer_type", "MPLANE");
        cfg->cap.use_mplane = (strcmp(bt, "MPLANE") == 0);
    }

    /* inference */
    {
        const char* v = kv_get("inference.enabled", "true");
        cfg->inf.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->inf.model_path,  kv_get("inference.model_path",  "model/yolov5s.rknn"), 255);
    strncpy(cfg->inf.labels_path, kv_get("inference.labels_path", "model/coco_80_labels_list.txt"), 255);
    cfg->inf.conf_thresh = (float)atof(kv_get("inference.conf_threshold", "0.25"));
    cfg->inf.nms_thresh  = (float)atof(kv_get("inference.nms_threshold",  "0.45"));
    cfg->inf.model_w = (uint32_t)atoi(kv_get("inference.model_width",  "640"));
    cfg->inf.model_h = (uint32_t)atoi(kv_get("inference.model_height", "640"));
    {
        const char* v = kv_get("inference.quantized", "true");
        cfg->inf.quantized = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    cfg->inf.npu_core = (uint32_t)atoi(kv_get("inference.npu_core", "0"));

    /* encode */
    {
        const char* v = kv_get("encode.enabled", "true");
        cfg->enc.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->enc.codec,    kv_get("encode.codec",    "h264"), 15);
    cfg->enc.bitrate  = (uint32_t)atoi(kv_get("encode.bitrate",  "4000000"));
    cfg->enc.gop_size = (uint32_t)atoi(kv_get("encode.gop_size", "60"));

    /* stream */
    {
        const char* v = kv_get("stream.enabled", "false");
        cfg->strm.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->strm.url, kv_get("stream.url", "rtmp://127.0.0.1/live/stream"), 255);
    {
        const char* v = kv_get("stream.reconnect", "true");
        cfg->strm.reconnect = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    cfg->strm.reconnect_delay_ms = (uint32_t)atoi(kv_get("stream.reconnect_delay", "2000"));
    cfg->strm.max_reconnect      = (int32_t)atoi(kv_get("stream.max_reconnect", "10"));

    /* display */
    {
        const char* v = kv_get("display.enabled", "true");
        cfg->disp.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->disp.window_name, kv_get("display.window_name", "RK3568 Vision"), 63);
    {
        const char* v = kv_get("display.show_fps", "true");
        cfg->disp.show_fps = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }

    /* monitor */
    {
        const char* v = kv_get("monitor.enabled", "true");
        cfg->mon.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    cfg->mon.log_interval_ms = (uint32_t)atoi(kv_get("monitor.log_interval", "5000"));

    return 0;
}
