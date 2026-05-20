/*
 * ==========================================================================
 * pipeline.c — 流水线核心编排模块
 * ==========================================================================
 *
 * **整体架构：生产者-消费者模型（Producer-Consumer Pattern）**
 *
 *   这是一个经典的"多级流水线"架构，每级一个独立线程：
 *
 *   ┌──────────┐       ┌──────────┐       ┌──────────┐
 *   │ 采集线程  │──cap_q──→│ 推理线程  │──inf_q──→│ 编码线程  │
 *   │ V4L2     │       │ RKNN     │       │ FFmpeg   │
 *   │ 30fps    │       │ ~25ms/帧 │       │ H.264    │
 *   └──────────┘       └────┬─────┘       └──────────┘
 *                           │ disp_q
 *                           ↓
 *                      ┌──────────┐
 *                      │ 显示线程  │
 *                      │ OpenCV   │
 *                      └──────────┘
 *
 *   线程间通过 SPSC 无锁环形队列 (ringbuf_t) 传递 frame_t* 指针：
 *     - cap_q: 采集 → 推理（容量 8）
 *     - inf_q: 推理 → 编码（容量 8）
 *     - disp_q: 推理 → 显示（容量 8，如果无推理则由采集直接推送）
 *
 * **为什么用四线程解耦？**
 *   1. 采集（~33ms/帧固定节奏）不受推理（~25ms 变时延）阻塞
 *   2. 推理不受编码（~5ms 变时延）阻塞
 *   3. 显示线程独立，不影响核心采集→推理→编码管道
 *   4. 各线程可独立调整优先级和 CPU 亲和性
 *
 * **引用计数帧共享（frame_t.refcount）**
 *   同一帧数据需要被多个消费者使用时（推理结果发给编码+显示），
 *   通过 frame_ref 增加引用计数，frame_free 减少引用计数，
 *   只有计数归零时才真正释放内存 → 避免数据拷贝
 *
 * **NV12→BGR 惰性转换（Lazy Conversion）**
 *   显示线程需要 BGR 格式（OpenCV 显示），编码线程直接使用 NV12（FFmpeg）
 *   转换只在第一次需要时执行（bgr_valid 标志），后续直接复用
 *   编码线程全程使用 NV12，不需要 BGR，节省大量计算
 */


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

#define QUEUE_CAP 8   /* 每个环形队列的实际可用容量 */

/*
 * 流水线主结构体
 * 持有所有子模块、环形队列、线程句柄、配置等
 * 是系统的"中央调度器"
 */
struct pipeline_s {
    app_cfg_t     cfg;         /* 完整配置（包含采集/推理/编码/推流/显示所有参数） */
    v4l2_cap_t*   v4l2;       /* V4L2 采集器实例                      */
    encoder_t*    enc;         /* FFmpeg H.264 编码器实例               */
    rtmp_t*       rtmp;        /* RTMP 推流器实例                      */
    void*         detector;    /* 检测器实例（bridge 中的 C++ Detector） */
    void*         display;     /* 显示窗口实例（cv::String）            */

    /* ── 帧池（预分配 frame_t 数组，避免运行时 malloc） ───────────── */
    void*    frames[PIPELINE_FRAME_POOL];  /* frame_t* 池 */
    int      pool_n;

    /* ── 三个 SPSC 无锁环形队列 ─────────────────────────────────── */
    /*
     * 队列数组大小 = QUEUE_CAP + 1
     * 额外 +1 用于 ringbuf 内部区分队空和队满（哨兵槽位）
     */
    void*    cap_q_buf[QUEUE_CAP + 1];     /* cap_q 存储数组 */
    ringbuf_t cap_q;                       /* 采集 → 推理队列 */

    void*    inf_q_buf[QUEUE_CAP + 1];     /* inf_q 存储数组 */
    ringbuf_t inf_q;                       /* 推理 → 编码队列 */

    void*    disp_q_buf[QUEUE_CAP + 1];    /* disp_q 存储数组 */
    ringbuf_t disp_q;                      /* 推理 → 显示队列（无推理时采集 → 显示） */

    /* ── 四个工作线程 ──────────────────────────────────────────── */
    pthread_t   th_cap;                   /* 采集线程 */
    pthread_t   th_inf;                   /* 推理线程 */
    pthread_t   th_enc;                   /* 编码线程 */
    pthread_t   th_disp;                  /* 显示线程 */

    int         running;                   /* 流水线运行标志（原子操作可写） */
};


/* ==========================================================================
 *  帧生命周期管理（引用计数）
 * ========================================================================== */

/*
 * 释放帧并递减引用计数
 * 只有当 refcount 降为 0 时才真正释放内存
 *
 * 原子操作 __atomic_sub_fetch 保证多线程并发调用安全
 * 例如：推理线程调 frame_free（-1），显示线程调 frame_free（-1）
 *       最后一个调用的线程看到 rc == 0，执行实际释放
 */
static void frame_free(frame_t* f) {
    if (!f) return;
    int rc = __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
    if (rc > 0) return;         /* 仍有其他引用持有者，不能释放 */
    free(f->data);               /* 释放 NV12 数据                       */
    free(f->bgr_data);           /* 释放 BGR 转换缓存（如果存在）       */
    free(f);                     /* 释放 frame_t 结构体本身              */
}

/*
 * 增加帧引用计数
 * 当一个新消费者需要读取某帧时调用
 * 例如：推理结果帧需要同时发给编码线程和显示线程时
 *       先 frame_ref(f) 再 push 到 disp_q
 */
static void frame_ref(frame_t* f) {
    if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
}


/* ==========================================================================
 *  流水线创建
 * ========================================================================== */

/*
 * 创建并初始化整个流水线
 *
 * 初始化顺序：
 *   1. 分配 pipeline_t 结构体
 *   2. 初始化三个环形队列
 *   3. 创建 V4L2 采集器
 *   4. 创建检测器（如果推理启用）
 *   5. 创建编码器（如果编码启用）
 *   6. 创建 RTMP 推流器（如果推流启用且编码器存在）
 *   7. 创建显示窗口（如果显示启用）
 *
 * 各模块创建失败不会导致整个流水线失败，而是降级运行
 * 例如：检测器加载模型失败 → 关闭推理，但采集+编码仍可正常工作
 */
pipeline_t* pipeline_create(const app_cfg_t* cfg) {
    pipeline_t* p = calloc(1, sizeof(pipeline_t));
    if (!p) return NULL;
    p->cfg = *cfg;               /* *cfg 做了解引用，复制配置（值拷贝，避免悬空指针）, */

    /*
     * 初始化三个环形队列
     * 队列只存储 void* 指针，不存储数据本身 → 零数据拷贝
     * 容量 QUEUE_CAP=8，允许最高 8 帧缓存（约 266ms @ 30fps 缓冲）
     */
    ringbuf_init(&p->cap_q,  (void**)p->cap_q_buf,  QUEUE_CAP);
    ringbuf_init(&p->inf_q,  (void**)p->inf_q_buf,  QUEUE_CAP);
    ringbuf_init(&p->disp_q, (void**)p->disp_q_buf, QUEUE_CAP);

    /* ── 创建 V4L2 采集器 ─────────────────────────────────────── */
    p->v4l2 = v4l2_open(cfg->cap.device, cfg->cap.width, cfg->cap.height,
                         cfg->cap.fps, cfg->cap.pixfmt, cfg->cap.buf_count);
    if (!p->v4l2) { LOG_ERROR("v4l2_open failed"); goto fail; }

    /* ── 创建检测器（C++ Detector，通过 bridge 封装为 C 接口） ─── */
    if (cfg->inf.enabled) {
        p->detector = bridge_detector_create();
        if (!p->detector || !bridge_detector_init(p->detector,
                cfg->inf.model_path, cfg->inf.labels_path,
                cfg->inf.conf_thresh, cfg->inf.nms_thresh, cfg->inf.npu_core)) {
            LOG_WARN("detector init failed, disabling inference");
            p->cfg.inf.enabled = 0;   /* 降级：关闭推理 */
        }
    }

    /* ── 创建编码器 ──────────────────────────────────────────── */
    if (cfg->enc.enabled) {
        p->enc = encoder_open(cfg->cap.width, cfg->cap.height, cfg->cap.fps,
                               cfg->enc.bitrate, cfg->enc.gop_size);
        if (!p->enc) { LOG_WARN("encoder init failed, disabling encode"); p->cfg.enc.enabled = 0; }
    }

    /* ── 创建 RTMP 推流器 ─────────────────────────────────────── */
    /* 只在编码器和推流都启用时创建 */
    if (cfg->strm.enabled && p->enc) {
        p->rtmp = rtmp_open(cfg->strm.url, cfg->cap.width, cfg->cap.height,
                            cfg->cap.fps, cfg->enc.bitrate);
        if (!p->rtmp) LOG_WARN("rtmp connect failed");  /* 非致命：编码继续但不推流 */
    }

    /* ── 创建 OpenCV 显示窗口 ─────────────────────────────────── */
    if (cfg->disp.enabled) {
        p->display = bridge_display_create(cfg->disp.window_name);
        if (!p->display) { LOG_WARN("display init failed, disabling display"); p->cfg.disp.enabled = 0; }
    }

    return p;

fail:
    pipeline_stop(p);            /* 清理已分配的资源 */
    return NULL;
}


/* ==========================================================================
 *  采集线程（Producer）
 * ========================================================================== */

/*
 * 采集线程主循环
 *
 * 职责：
 *   1. 循环调用 v4l2_capture() 采集帧
 *   2. 记录采集时间戳
 *   3. 将帧推入 cap_q（如果推理启用）或直接推入 inf_q/disp_q（如果无推理）
 *   4. 队列满时释放该帧并记录丢帧
 *
 * 数据流：
 *   [推理启用]  采集帧 → cap_q → 推理线程
 *   [推理禁用]  采集帧 → inf_q → 编码线程
 *                     → disp_q → 显示线程
 */
static void* capture_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("capture thread started");

    while (p->running) {
        /* 采集一帧（epoll 等待 + DQBUF + memcpy NV12 → frame_t） */
        frame_t* f = v4l2_capture(p->v4l2);
        if (!f) {
            if (sig_shutdown()) break;
            continue;            /* 超时或临时错误，继续下一轮 */
        }
        fps_tick();              /* 采集帧率统计 */
        f->cap_ts = timestamp_now(); /* 记录采集完成时间戳 */

        int use_inf = p->cfg.inf.enabled && p->detector;

        if (use_inf) {
            /*
             * 推理启用：帧推入 cap_q 给推理线程
             * 如果 cap_q 满（推理线程处理不过来了），丢弃帧
             * 这种"背压丢弃"策略保证系统不会无限堆积内存
             */
            if (!ringbuf_push(&p->cap_q, f)) {
                frame_free(f);
                perf_record_drop();
            }
        } else {
            /*
             * 推理禁用：跳过推理，直接推入编码队列和显示队列
             * frame_ref 用于多消费者场景：编码和显示都需要此帧
             * 首次 push 使用原始引用（refcount=1），第二次 push 前先 +1
             */
            if (!ringbuf_push(&p->inf_q, f)) {
                frame_free(f);
                perf_record_drop();
                continue;
            }
            if (p->cfg.disp.enabled) {
                frame_ref(f);    /* 显示线程也需要此帧 → refcount +1 */
                if (!ringbuf_push(&p->disp_q, f)) {
                    frame_free(f); /* 显示队列满则放弃显示，不影响编码 */
                    perf_record_drop();
                }
            }
        }
        if (sig_shutdown()) break;
    }
    LOG_INFO("capture thread stopped");
    return NULL;
}

/*
 * 纯C实现的高效 NV12→BGR 颜色空间转换（备选方案）
 *
 * 当前项目主要使用 OpenCV 的 cvtColor 进行转换（参见 bridge.cpp），
 * 但保留此纯 C 实现作为了解底层算法的参考
 *
 * NV12 格式：Y 平面（w×h）+ UV 交错平面（w×h/2）
 * 每个 2×2 的 Y 块共享一组 UV 值（4:2:0 色度子采样）
 *
 * 转换公式（ITU-R BT.601 标准，整数运算优化）：
 *   R = Y + 1.402 * (V - 128)
 *   G = Y - 0.344 * (U - 128) - 0.714 * (V - 128)
 *   B = Y + 1.772 * (U - 128)
 *
 * 为避免浮点运算（ARM 上浮点很慢），使用定点移位：
 *   R = Y + (359 * (V - 128) >> 8)
 *   G = Y - (88 * (U - 128) >> 8) - (183 * (V - 128) >> 8)
 *   B = Y + (454 * (U - 128) >> 8)
 */
static void nv12_to_bgr(frame_t* f) {
    if (f->bgr_valid) return;    /* 已转换过，直接复用 */
    int w = f->width, h = f->height;
    int stride = f->stride ? (int)f->stride : w;  /* 使用实际 stride */
    int bgr_size = w * h * 3;
    if (!f->bgr_data) f->bgr_data = malloc(bgr_size);
    if (!f->bgr_data) return;

    /* NV12 平面指针：Y 从 data 开始，UV 从 data + stride*h 开始 */
    uint8_t* y  = f->data;
    uint8_t* uv = f->data + stride * h;

    /* 逐像素转换（O(w×h) 复杂度） */
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            int yi  = i * stride + j;           /* Y 平面索引 */
            int uvi = (i / 2) * stride + (j & ~1); /* UV 平面索引（每2×2块共享） */
            int Y = y[yi];
            int U = uv[uvi] - 128;              /* 去中心化：U 分量偏移 -128 */
            int V = uv[uvi + 1] - 128;          /* 去中心化：V 分量偏移 -128 */

            /* ITU-R BT.601 YUV→RGB 定点运算（>>8 代替 /256） */
            int r = Y + ((359 * V) >> 8);
            int g = Y - ((88 * U + 183 * V) >> 8);
            int b = Y + ((454 * U) >> 8);

            /* clamp 到 [0, 255] 范围，BGR 顺序（OpenCV 默认） */
            int bi = (i * w + j) * 3;
            f->bgr_data[bi + 0] = b < 0 ? 0 : (b > 255 ? 255 : b);
            f->bgr_data[bi + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
            f->bgr_data[bi + 2] = r < 0 ? 0 : (r > 255 ? 255 : r);
        }
    }
    f->bgr_valid = 1;            /* 标记为已转换 */
}


/* ==========================================================================
 *  推理线程（Consumer + Producer）
 * ========================================================================== */

/*
 * 推理线程主循环
 *
 * 这是"消费-生产"型线程：
 *   消费：从 cap_q 取出采集帧
 *   生产：推入 inf_q（编码器消费）和 disp_q（显示器消费）
 *
 * 处理流程：
 *   1. ringbuf_pop(&p->cap_q) → 阻塞等待新帧
 *   2. NV12 → BGR 颜色空间转换（bridge_nv12_to_bgr，使用 OpenCV）
 *   3. YOLOv5 目标检测（bridge_detector_detect）
 *      3a. 预处理（BGR→RGB, resize 到 640×640）
 *      3b. RKNN 设置输入
 *      3c. NPU 硬件推理（rknn_run）
 *      3d. 后处理（INT8 反量化 + 解码框 + NMS）
 *   4. 记录推理时间戳和延迟
 *   5. 推入 inf_q（编码）和 disp_q（显示，如果启用）
 *
 * 延迟：目标单帧推理 < 30ms（NPU 推理 ~25ms + 前后处理 ~5ms）
 */
static void* inference_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("inference thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->cap_q);
        if (!f) { usleep(1000); if (sig_shutdown()) break; continue; }

        int64_t t0 = (int64_t)timestamp_now();  /* 推理开始时间 */

        /* 惰性分配 BGR 缓冲区（如果还没分配） */
        if (!f->bgr_data) f->bgr_data = malloc((size_t)f->width * f->height * 3);
        if (!f->bgr_data) { frame_free(f); continue; }

        /*
         * NV12 → BGR 转换（使用 OpenCV cvtColor，SIMD 加速）
         * 转换结果存储在 f->bgr_data 中，标记 bgr_valid=1
         */
        bridge_nv12_to_bgr(f->data, (int)f->stride, (int)f->width, (int)f->height, f->bgr_data);
        f->bgr_valid = 1;

        /*
         * 执行 YOLOv5 目标检测
         * 结果写入 f->detect（detect_result_t）
         */
        bridge_detector_detect(p->detector, f->bgr_data,
                                f->width, f->height, &f->detect);
        f->inf_ts = timestamp_now();  /* 推理完成时间戳 */
        perf_record_infer((int64_t)(f->inf_ts - t0));

        /* 每 60 帧输出一次检测摘要（减少日志洪水） */
        if ((f->seq % 60) == 0)
            LOG_INFO("frame %llu: %u det, %.0fms",
                     (unsigned long long)f->seq, f->detect.count,
                     (f->inf_ts - t0) / 1000.0);

        /*
         * 将推理结果帧推入编码队列
         * 编码线程从中取帧进行 H.264 编码
         */
        ringbuf_push(&p->inf_q, f);

        /*
         * 如果显示启用，额外推入显示队列
         * 注意：frame_ref 增加引用计数，因为编码和显示都需要访问此帧
         */
        if (p->cfg.disp.enabled) {
            frame_ref(f);
            if (!ringbuf_push(&p->disp_q, f))
                frame_free(f);   /* 显示队列满则放弃显示 */
        }

        if (sig_shutdown()) break;
    }
    LOG_INFO("inference thread stopped");
    return NULL;
}


/* ==========================================================================
 *  RTMP 回调函数
 * ========================================================================== */

/*
 * 编码器输出回调 — 编码器每产生一个 H.264 包就调用此函数
 * 将编码数据推送到 RTMP 服务器
 *
 * @userdata：rtmp_t* 指针
 * @data：H.264/H.265 编码数据（NAL unit / slice）
 * @size：数据字节数
 * @pts：显示时间戳（来自 frame_t.seq）
 * @keyframe：是否为关键帧（I 帧）
 */
static void rtmp_callback(void* userdata, const uint8_t* data,
                           size_t size, int64_t pts, int keyframe) {
    rtmp_t* r = (rtmp_t*)userdata;
    if (r && rtmp_is_connected(r))
        rtmp_push_video(r, data, size, pts, keyframe);
}


/* ==========================================================================
 *  编码线程（Consumer）
 * ========================================================================== */

/*
 * 编码线程主循环
 *
 * 消费 inf_q 中的帧（可能来自推理线程或采集线程），编码为 H.264 格式
 * 
 * 编码器配置：
 *   - 输入：NV12 格式（直接从 frame_t.data 读取）
 *   - 内部：swscale 转换 NV12 → YUV420P
 *   - 输出：H.264 码流（通过回调发给 RTMP 推流器）
 *
 * sleep 策略：
 *   - 队列空时 usleep(5000) = 5ms
 *   - 这比推理线程（usleep(1000)）更长，因为编码通常很快（~5ms/帧）
 *   - 不同 sleep 时间可以调节 CPU 使用率
 */
static void* encode_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("encode thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->inf_q);
        if (!f) { usleep(5000); if (sig_shutdown()) break; continue; }

        /*
         * 编码帧：传入 NV12 数据和帧序号作为 PTS
         * 编码器内部自动调用 sws_scale 转换格式
         * 编码完成后通过 rtmp_callback 将 H.264 码流推送到 RTMP
         */
        encoder_encode(p->enc, f->data, (int64_t)f->seq);
        fps_tick();              /* 编码帧率统计 */
        frame_free(f);           /* 编码完成，释放帧 */
        if (sig_shutdown()) break;
    }
    LOG_INFO("encode thread stopped");
    return NULL;
}


/* ==========================================================================
 *  显示线程（Consumer）
 * ========================================================================== */

/*
 * 显示线程主循环
 *
 * 消费 disp_q 中的帧，通过 OpenCV 显示
 * 
 * 显示内容：
 *   - 原始视频画面（NV12→BGR 转换后）
 *   - FPS 叠加文字（如果 show_fps=true）
 *   - 检测框和标签（如果推理启用且有检测结果）
 *
 * 惰性转换：
 *   - 如果 f->bgr_valid 已为 true（推理线程已转换过），直接使用
 *   - 否则执行 NV12→BGR 转换（无推理直接显示采集帧的场景）
 */
static void* display_thread(void* arg) {
    pipeline_t* p = (pipeline_t*)arg;
    fps_reset();
    LOG_INFO("display thread started");

    while (p->running) {
        frame_t* f = ringbuf_pop(&p->disp_q);
        if (!f) { usleep(1000); if (sig_shutdown()) break; continue; }

        /* 惰性转换：如果推理线程已转换过，跳过；否则自行转换 */
        if (!f->bgr_valid) {
            if (!f->bgr_data) f->bgr_data = malloc((size_t)f->width * f->height * 3);
            if (f->bgr_data) {
                bridge_nv12_to_bgr(f->data, (int)f->stride, (int)f->width, (int)f->height, f->bgr_data);
                f->bgr_valid = 1;
            }
        }

        /* 显示帧（叠加 FPS/检测框） */
        if (f->bgr_valid) {
            bridge_display_show(p->display, f->bgr_data,
                                 f->width, f->height,
                                 p->cfg.disp.show_fps, fps_get(), &f->detect);
        }
        fps_tick();              /* 显示帧率统计 */
        frame_free(f);           /* 显示完成，释放帧 */

        if (sig_shutdown()) break;
    }
    LOG_INFO("display thread stopped");
    return NULL;
}


/* ==========================================================================
 *  流水线生命周期管理
 * ========================================================================== */

/*
 * 启动流水线
 *
 * 启动顺序（有依赖关系）：
 *   1. V4L2_STREAMON：启动摄像头硬件流
 *   2. encoder_set_callback：设置编码输出回调（编码数据→RTMP）
 *   3. 创建四个工作线程（pthread_create）
 *
 * 线程创建根据配置决定：
 *   - 推理线程：仅当 inf.enabled && detector 存在时创建
 *   - 编码线程：仅当 enc.enabled && enc 存在时创建
 *   - 显示线程：仅当 disp.enabled && display 存在时创建
 */
int pipeline_start(pipeline_t* p) {
    if (!p) return 0;

    /* 启动摄像头采集流 */
    if (v4l2_start(p->v4l2) < 0) { LOG_ERROR("v4l2_start failed"); return 0; }

    /* 设置编码器输出回调：H.264 包 → RTMP 推流 */
    if (p->enc && p->rtmp)
        encoder_set_callback(p->enc, rtmp_callback, p->rtmp);

    p->running = 1;

    /* 创建采集线程（总是需要） */
    pthread_create(&p->th_cap,  NULL, capture_thread,    p);
    /* 创建推理线程（条件创建） */
    if (p->cfg.inf.enabled && p->detector)
        pthread_create(&p->th_inf,  NULL, inference_thread,   p);
    /* 创建编码线程（条件创建） */
    if (p->cfg.enc.enabled && p->enc)
        pthread_create(&p->th_enc,  NULL, encode_thread,      p);
    /* 创建显示线程（条件创建） */
    if (p->cfg.disp.enabled && p->display)
        pthread_create(&p->th_disp, NULL, display_thread,     p);

    LOG_INFO("pipeline started");
    return 1;
}

/*
 * 停止并销毁流水线
 *
 * 关闭顺序（必须严格遵循，防止资源泄漏和竞态条件）：
 *   1. 设置 running=0（通知所有线程退出）
 *   2. pthread_join 等待所有线程结束
 *   3. 关闭 V4L2 设备（先 STOP STREAMOFF, 再 munmap, 再 close fd）
 *   4. flush 编码器（清空编码缓冲区中的残留帧）
 *   5. 关闭编码器
 *   6. 关闭 RTMP 推流器
 *   7. 销毁检测器和显示窗口
 *   8. 排空所有环形队列中的残留帧（释放内存）
 *   9. 释放 pipeline_t 结构体
 *
 * 关键：必须先 join 线程再释放资源，否则线程可能访问已释放的内存
 */
void pipeline_stop(pipeline_t* p) {
    if (!p) return;
    if (!p->running) { free(p); return; }
    p->running = 0;              /* 通知所有线程退出循环 */

    /* 等待所有线程正常退出 */
    pthread_join(p->th_cap,  NULL);
    if (p->cfg.inf.enabled && p->detector)  pthread_join(p->th_inf,  NULL);
    if (p->cfg.enc.enabled && p->enc)       pthread_join(p->th_enc,  NULL);
    if (p->cfg.disp.enabled && p->display)  pthread_join(p->th_disp, NULL);

    /* 按创建顺序的反序释放资源 */
    if (p->v4l2) { v4l2_stop(p->v4l2); v4l2_close(p->v4l2); p->v4l2 = NULL; }
    if (p->enc)  { encoder_flush(p->enc); encoder_close(p->enc); p->enc = NULL; }
    if (p->rtmp) { rtmp_close(p->rtmp); p->rtmp = NULL; }
    if (p->detector) { bridge_detector_destroy(p->detector); p->detector = NULL; }
    if (p->display)  { bridge_display_destroy(p->display); p->display = NULL; }

    /* 排空环形队列：释放残留帧（如果线程异常退出可能有残留） */
    frame_t* f;
    while ((f = ringbuf_pop(&p->cap_q)))  frame_free(f);
    while ((f = ringbuf_pop(&p->inf_q)))  frame_free(f);
    while ((f = ringbuf_pop(&p->disp_q))) frame_free(f);

    LOG_INFO("pipeline stopped");
    free(p);
}

/* 检查流水线是否正在运行 */
int pipeline_running(pipeline_t* p) { return p && p->running; }
