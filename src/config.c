/*
 * ==========================================================================
 * config.c — YAML 配置文件解析器（精简实现）
 * ==========================================================================
 *
 * **设计决策：为什么不使用 libyaml？**
 *   - 本项目配置文件非常简单（键值对，无嵌套列表/锚点/引用）
 *   - 引入 libyaml 会增加编译依赖和链接复杂度
 *   - 用约 200 行 C 代码实现一个足够用的精简 YAML 解析器
 *   - 支持的功能：顶级键、嵌套键（用 . 分隔）、字符串/布尔/数字值、注释
 *
 * **解析格式**：
 *   配置项按 "section.key" 格式存储在键值表中
 *   例如 YAML 中：
 *     capture:
 *       device: "/dev/video0"
 *   解析为：capture.device = "/dev/video0"
 *
 * **支持的 YAML 特性**：
 *   - 缩进表示层级关系（顶级/子级）
 *   - 键值对用冒号分隔（key: value）
 *   - 字符串值可用双引号或单引号包裹
 *   - # 开头的行视为注释
 *   - 空行忽略
 *
 * **不支持的特性**：
 *   - 列表（YAML 数组语法）
 *   - 多行字符串
 *   - 锚点和引用（&anchor, *alias）
 *   - JSON 风格的流式语法
 */

#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEY_LEN   128     /* 配置键最大长度 */
#define MAX_VAL_LEN   256     /* 配置值最大长度 */
#define MAX_ENTRIES   128     /* 最大配置项数量 */

/* 键值对结构体 */
typedef struct { char key[MAX_KEY_LEN]; char val[MAX_VAL_LEN]; } kv_t;

/* 全局键值表（一次程序运行只加载一次配置，用全局变量即可） */
static kv_t   g_kv[MAX_ENTRIES];
static size_t g_kv_n = 0;

/* 存储键值对到表中 */
static void kv_set(const char* key, const char* val) {
    if (g_kv_n >= MAX_ENTRIES) return;
    strncpy(g_kv[g_kv_n].key, key, MAX_KEY_LEN - 1);
    strncpy(g_kv[g_kv_n].val, val, MAX_VAL_LEN - 1);
    g_kv_n++;
}

/* 从表中查找键对应值，找不到返回默认值 def */
static const char* kv_get(const char* key, const char* def) {
    for (size_t i = 0; i < g_kv_n; i++)
        if (strcmp(g_kv[i].key, key) == 0) return g_kv[i].val;
    return def;
}

/* 清空键值表（重新加载配置前调用） */
static void kv_clear(void) { g_kv_n = 0; }


/* ==========================================================================
 *  精简 YAML 解析器
 * ========================================================================== */

/* 去除字符串右侧空白字符（空格、制表符、回车、换行） */
static void trim_right(char* s) {
    char* end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
}

/* 跳过字符串左侧空白字符（原地修改指针） */
static void trim_left(char** ps) {
    while (**ps == ' ' || **ps == '\t') (*ps)++;
}

/* 去除 # 及其后的注释内容（原地截断字符串） */
static void strip_comment(char* s) {
    char* c = strchr(s, '#');
    if (c) *c = '\0';
}

/* 去除字符串两端的引号（双引号或单引号） */
static void trim_quotes(char* s) {
    int len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len-1] == '"') || (s[0] == '\'' && s[len-1] == '\''))) {
        memmove(s, s + 1, len - 2);  /* 左移 1 位覆盖开头引号 */
        s[len - 2] = '\0';           /* 截断结尾引号 */
    }
}

/*
 * 解析 YAML 文件中的一行
 *
 * 行类型判断：
 *   1. 以字母开头 + 冒号结尾 → 这是一个 Section 声明（如 "capture:"）
 *      更新 section 字符串（缩进表示层级）
 *   2. 包含冒号在中间 → 这是一个键值对（如 "  device: /dev/video0"）
 *      构造完整键名 "section.key" 存入键值表
 *
 * @line：一行原始文本
 * @section：当前所在的 section 路径（如 "capture.sub"）
 * @indent：当前 section 的缩进级别
 */
static void parse_line(char* line, char* section, int* indent) {
    char* p = line;
    trim_left(&p);            /* 跳过行首空白 */
    if (*p == '\0') return;   /* 空行，忽略 */

    int new_indent = (int)(p - line);  /* 计算缩进空格数 */
    strip_comment(p);         /* 去除注释 */
    trim_right(p);            /* 去除行尾空白 */
    if (*p == '\0') return;   /* 纯注释行，忽略 */

    int len = (int)strlen(p);
    if (p[len - 1] == ':') {
        /* 这是一个 Section 声明（如 "capture:" 或 "  sub:"） */
        p[len - 1] = '\0';    /* 去除结尾冒号 */
        trim_right(p);
        if (new_indent == 0)
            /* 顶级 section：直接替换（如 "capture:"） */
            strncpy(section, p, MAX_KEY_LEN - 1);
        else if (section[0]) {
            /*
             * 嵌套 section：追加到当前路径
             * 注意：这只是简单追加，不支持"缩进回退"（回到父级）
             * 对于本项目配置文件的嵌套深度（最多1层），简单追加足够
             * 例如 section="capture" + 缩进2空格的新行 "sub:" → section="capture.sub"
             */
            char tmp[MAX_KEY_LEN];
            snprintf(tmp, sizeof(tmp), "%s.%s", section, p);
            strncpy(section, tmp, MAX_KEY_LEN - 1);
        } else
            strncpy(section, p, MAX_KEY_LEN - 1);
        *indent = new_indent;
        return;
    }

    /* 这是一个键值对（如 "device: /dev/video0"） */
    char* colon = strchr(p, ':');
    if (!colon) return;       /* 格式错误，忽略该行 */
    *colon = '\0';            /* 分割键和值 */
    char* key = p;
    char* val = colon + 1;
    trim_right(key);          /* 去除键尾空白 */
    trim_left(&val);          /* 去除值首空白 */
    trim_quotes(val);         /* 去除值的引号 */

    /* 构造完整键名 "section.key" */
    char full_key[MAX_KEY_LEN];
    if (section[0])
        snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
    else
        strncpy(full_key, key, sizeof(full_key) - 1);

    kv_set(full_key, val);    /* 存入键值表 */
}

/*
 * 加载 YAML 配置文件并填充 app_cfg_t 结构体
 *
 * 流程：
 *   1. 清空键值表
 *   2. 逐行读取 YAML 文件，调用 parse_line 解析
 *   3. 从键值表中读取各项配置，填充到 app_cfg_t
 *   4. 如果文件不存在，使用默认值（不报错，降级运行）
 *
 * 命令行参数可以覆盖 YAML 中的默认值（在 main.c 中处理）
 *
 * @yaml_path：配置文件路径
 * @cfg：输出参数，填充的配置结构体
 * 返回：0 成功
 */
int config_load(const char* yaml_path, app_cfg_t* cfg) {
    kv_clear();

    FILE* f = fopen(yaml_path, "r");
    if (!f) {
        LOG_WARN("config file not found: %s, using defaults", yaml_path);
        /* 文件不存在不报错，降级使用默认值（通过 kv_get 的 def 参数） */
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

    /* 清空 cfg 结构体（所有字段初始化为 0） */
    memset(cfg, 0, sizeof(*cfg));

    /* ── 采集配置 ─────────────────────────────────────────────── */
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

    /* ── 推理配置 ─────────────────────────────────────────────── */
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

    /* ── 编码配置 ─────────────────────────────────────────────── */
    {
        const char* v = kv_get("encode.enabled", "true");
        cfg->enc.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->enc.codec,    kv_get("encode.codec",    "h264"), 15);
    cfg->enc.bitrate  = (uint32_t)atoi(kv_get("encode.bitrate",  "4000000"));
    cfg->enc.gop_size = (uint32_t)atoi(kv_get("encode.gop_size", "60"));

    /* ── 推流配置 ─────────────────────────────────────────────── */
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

    /* ── 显示配置 ─────────────────────────────────────────────── */
    {
        const char* v = kv_get("display.enabled", "true");
        cfg->disp.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    strncpy(cfg->disp.window_name, kv_get("display.window_name", "RK3568 Vision"), 63);
    {
        const char* v = kv_get("display.show_fps", "true");
        cfg->disp.show_fps = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }

    /* ── 监控配置 ─────────────────────────────────────────────── */
    {
        const char* v = kv_get("monitor.enabled", "true");
        cfg->mon.enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    cfg->mon.log_interval_ms = (uint32_t)atoi(kv_get("monitor.log_interval", "5000"));

    return 0;
}
