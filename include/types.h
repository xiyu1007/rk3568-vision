#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 全项目共用的基础类型定义
 * 所有模块（采集/推理/编码/推流/显示）都通过这里定义的结构体交换数据
 * ========================================================================== */

/* ── Timestamp 时间戳（微秒，自系统启动起算） ─────────────────────────────── */
/*
 * 使用 CLOCK_MONOTONIC 而非 CLOCK_REALTIME，因为：
 *   - 单调时钟不会受系统时间调整影响（避免 NTP 校时导致的时间跳变）
 *   - 用于测量延迟时，单调时钟保证时间差永远为正
 *   - 微秒精度足够满足毫秒级性能分析需求
 */
typedef uint64_t timestamp_us_t;

static inline timestamp_us_t timestamp_now(void);

/* ── Frame buffer 帧缓冲区（NV12 原始数据 + 元数据） ─────────────────────── */
/*
 * NV12 格式内存布局：
 *   [Y 平面]  ← w×h 字节（亮度，每个像素一个字节）
 *   [UV 平面] ← (w×h)/2 字节（色度，UV 交错存储，每4个Y共享一组UV）
 *   总大小 = w*h + w*h/2 = w*h*1.5
 *
 * 为什么选 NV12：
 *   1. RK3568 ISP 硬件直接输出 NV12，无需软件转换
 *   2. RKNN NPU 可直接消费 NV12（内部有硬件预处理单元）
 *   3. FFmpeg x264 编码器可通过 swscale 高效转换 NV12→YUV420P
 */

#define FRAME_MAX_PLANES 4    /* V4L2 最多支持4个内存平面 */
#define FRAME_LABEL_MAX  64   /* 检测标签字符串最大长度（如 "person"） */
#define DETECT_MAX_BOXES 64   /* 单帧最多输出64个检测框 */

/* ── Detection result 检测结果 ─────────────────────────────────────────── */
/*
 * 每个检测框对应一个识别到的目标
 * x,y,w,h：边界框在原始图像坐标系中的位置和尺寸（像素）
 * class_id：类别编号（0=person, 1=bicycle, ...对应 COCO 80 类）
 * conf：置信度（0.0~1.0）
 * label：类别名称字符串
 */
typedef struct {
    int   x, y, w, h;
    int   class_id;
    float conf;
    char  label[FRAME_LABEL_MAX];
} detect_box_t;

/*
 * 一帧的完整检测结果
 * count：有效检测框数量（0~DETECT_MAX_BOXES）
 * boxes[]：检测框数组
 */
typedef struct {
    uint32_t     count;
    detect_box_t boxes[DETECT_MAX_BOXES];
} detect_result_t;

/* ── Frame 帧结构体（整个流水线的核心数据单元） ─────────────────────────── */
/*
 * 这是一个"重"结构体，贯穿采集→推理→编码→显示全流程
 * 
 * 关键设计：
 *   - refcount：引用计数，支持多消费者共享同一帧数据
 *     例如：推理完成后，同一帧要同时发给编码线程和显示线程
 *     此时 refcount 会 +1 再 +1，等两个线程都释放后才真正 free
 *   - bgr_data + bgr_valid：惰性转换（Lazy Conversion）
 *     NV12→BGR 转换在第一次需要时才执行，后续直接复用
 *     编码线程用 NV12（原生），显示线程用 BGR（OpenCV 需要）
 *   - cap_ts / inf_ts：采集时间戳和推理时间戳，用于计算端到端延迟
 */
typedef struct {
    uint8_t* data;           /* NV12 原始数据指针                      */
    size_t   size;           /* NV12 数据总字节数                      */
    uint32_t width;          /* 图像宽度（像素）                       */
    uint32_t height;         /* 图像高度（像素）                       */
    uint32_t stride;         /* Y 平面行步长（可能 > width，V4L2 对齐） */
    uint64_t seq;            /* 帧序号（单调递增，用于追踪和日志）     */
    timestamp_us_t cap_ts;   /* 采集完成时间戳（微秒）                 */
    timestamp_us_t inf_ts;   /* 推理完成时间戳（微秒）                 */
    uint8_t* bgr_data;       /* BGR 转换缓存（显示线程使用）          */
    bool     bgr_valid;      /* bgr_data 是否已转换且有效             */
    int      refcount;       /* 原子引用计数，为0时释放内存            */
    detect_result_t detect;  /* 该帧的检测结果                        */
} frame_t;

/* ── Performance counters 性能计数器 ──────────────────────────────────── */
/*
 * 全局性能统计，记录各阶段的最近一次延迟（微秒）
 * 所有字段使用原子操作读写，多线程安全
 * cap_us：采集延迟（DQBUF 耗时）
 * inf_us：推理延迟（预处理 + NPU 推理 + 后处理总耗时）
 * enc_us：编码延迟（NV12→YUV420P + x264 编码耗时）
 * total_frames：累计处理帧数
 * dropped_frames：累计丢帧数（环形队列满时丢弃）
 */
typedef struct {
    int64_t cap_us;          /* 采集延迟（最近一次）                   */
    int64_t inf_us;          /* 推理延迟（最近一次）                   */
    int64_t enc_us;          /* 编码延迟（最近一次）                   */
    int64_t total_frames;
    int64_t dropped_frames;
} perf_t;

/* ── Configuration structures 配置结构体 ──────────────────────────────── */
/*
 * 与 config/default.yaml 中的配置项一一对应
 * 命令行参数可覆盖 YAML 中的默认值
 */

/* 视频采集配置 */
typedef struct {
    char     device[64];         /* V4L2 设备节点路径，如 /dev/video0   */
    uint32_t width, height, fps; /* 采集分辨率与帧率                     */
    char     pixfmt[16];         /* 像素格式：NV12 / MJPEG / YUYV       */
    uint32_t buf_count;          /* DMA buffer 数量（4~8，建议6）       */
    int      use_mplane;         /* 是否使用多平面 API（V4L2 MPLANE）   */
} cap_cfg_t;

/* RKNN 推理配置 */
typedef struct {
    bool     enabled;            /* 是否启用推理                         */
    char     model_path[256];    /* .rknn 模型文件路径                   */
    char     labels_path[256];   /* 类别标签文件路径                     */
    float    conf_thresh;        /* 置信度阈值（低于此值丢弃）           */
    float    nms_thresh;         /* NMS IoU 阈值                        */
    uint32_t model_w, model_h;   /* 模型输入尺寸（如 640×640）          */
    bool     quantized;          /* 是否 INT8 量化（推荐）              */
    uint32_t npu_core;           /* NPU 核心号（RK3568 为 0）           */
} inf_cfg_t;

/* H.264 编码配置 */
typedef struct {
    bool     enabled;            /* 是否启用编码                         */
    char     codec[16];          /* 编码器名称：h264 / h265             */
    uint32_t bitrate;            /* 目标码率（bps），例 4000000 = 4Mbps  */
    uint32_t gop_size;           /* GOP 大小（关键帧间隔），60 表示每2秒一个I帧 */
} enc_cfg_t;

/* RTMP 推流配置 */
typedef struct {
    bool     enabled;            /* 是否启用推流                         */
    char     url[256];           /* RTMP 服务器地址                      */
    bool     reconnect;          /* 断流后是否自动重连                   */
    uint32_t reconnect_delay_ms; /* 重连间隔（毫秒）                     */
    int32_t  max_reconnect;      /* 最大重连次数（-1 表示无限）         */
} strm_cfg_t;

/* OpenCV 本地显示配置 */
typedef struct {
    bool     enabled;            /* 是否启用本地显示                     */
    char     window_name[64];    /* 窗口标题                            */
    bool     show_fps;           /* 是否在画面上叠加 FPS 文字            */
} disp_cfg_t;

/* 性能监控配置（CPU/内存/温度） */
typedef struct {
    bool     enabled;            /* 是否启用性能监控                     */
    uint32_t log_interval_ms;    /* 性能日志输出间隔（毫秒）             */
} mon_cfg_t;

/* 总配置结构体 */
typedef struct {
    cap_cfg_t  cap;              /* 采集配置                            */
    inf_cfg_t  inf;              /* 推理配置                            */
    enc_cfg_t  enc;              /* 编码配置                            */
    strm_cfg_t strm;             /* 推流配置                            */
    disp_cfg_t disp;             /* 显示配置                            */
    mon_cfg_t  mon;              /* 监控配置                            */
} app_cfg_t;

/* ── Configuration loading 配置加载 ───────────────────────────────────── */
int  config_load(const char* yaml_path, app_cfg_t* cfg);

/* ── Timestamp implementation 时间戳实现 ───────────────────────────────── */
/*
 * 获取自系统启动以来的微秒数
 * CLOCK_MONOTONIC：不受系统时间调整影响，适合测量时间间隔
 * tv_sec * 1,000,000 + tv_nsec / 1,000 = 微秒
 */
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
