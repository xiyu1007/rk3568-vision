/*
 * ==========================================================================
 * encoder.c — FFmpeg H.264 硬件编码模块
 * ==========================================================================
 *
 * **功能**：将 NV12 原始视频帧编码为 H.264 码流
 *
 * **为什么用 FFmpeg 而非 GStreamer？**
 *   - FFmpeg libavcodec 是 C 库，直接链接，无额外进程开销
 *   - 更轻量，启动更快（GStreamer 需要 pipeline 初始化）
 *   - 本项目使用 libx264 软件编码器
 *   - 未来可切换为 RK3568 的硬件编码器（rkmpp / h264_rkmpp）
 *     以进一步降低编码延迟和 CPU 占用
 *
 * **编码流程**：
 *   1. NV12 → YUV420P 格式转换（sws_scale）
 *   2. avcodec_send_frame()：将 YUV420P 帧送入编码器
 *   3. avcodec_receive_packet()：取出编码后的 H.264 包（NAL units）
 *   4. 通过回调函数将编码数据传递给 RTMP 推流器
 *
 * **NV12 与 YUV420P 的区别**：
 *   - NV12：Y 平面 + UV 交错平面（NV12），IMX415 ISP 直接输出
 *   - YUV420P：Y 平面 + U 平面 + V 平面（三个独立平面），x264 编码器需要
 *   - 两者在采样方式上相同（4:2:0），但平面排列不同
 *   - sws_scale 负责高效的格式转换（使用 SIMD 指令加速）
 *
 * **码率控制**：
 *   - CBR（Constant Bitrate）：通过 bit_rate 参数设置目标码率
 *   - GOP（Group of Pictures）：gop_size=60 表示每 60 帧一个关键帧（I 帧）
 *   - 关键帧的作用：解码器随机访问点，用于 RTMP 播放器快速启动
 *   - preset=fast：牺牲少量编码效率换取编码速度
 *   - profile=high：H.264 High Profile，支持更高效的编码
 */

#include "encoder.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>    /* AVCodec, AVCodecContext, AVPacket */
#include <libavutil/imgutils.h>     /* av_image_* 工具函数              */
#include <libavutil/opt.h>          /* av_opt_set（设置编码器参数）     */
#include <libswscale/swscale.h>     /* sws_scale（像素格式转换）        */

/*
 * 编码器内部状态
 * 封装 FFmpeg 编解码上下文、帧缓冲、格式转换器等
 */
struct encoder_s {
    AVCodecContext* ctx;           /* FFmpeg 编码器上下文（持有编码器状态） */
    AVFrame*        frame;         /* 用于送入编码器的 YUV420P 帧          */
    AVPacket*       pkt;           /* 用于接收编码器输出的 H.264 包        */
    struct SwsContext* sws;        /* 像素格式转换器：NV12 → YUV420P       */
    uint32_t        width, height; /* 视频分辨率                            */
    int64_t  frame_n;              /* 已编码帧计数（用于生成 PTS）         */
    enc_packet_cb cb;              /* 编码输出回调函数指针                  */
    void*    cb_data;              /* 回调函数的用户数据（传给 RTMP 推流器） */
};

/*
 * 打开并初始化 H.264 编码器
 *
 * 初始化流程：
 *   1. 查找 libx264 编码器（avcodec_find_encoder_by_name）
 *   2. 分配编码器上下文（avcodec_alloc_context3）
 *   3. 设置编码参数：分辨率、帧率、像素格式、码率、GOP
 *   4. 打开编码器（avcodec_open2）
 *   5. 分配 AVFrame 缓冲（YUV420P 格式）
 *   6. 创建格式转换器：NV12 → YUV420P（sws_getContext）
 *
 * @w, h：视频分辨率
 * @fps：帧率（用于 time_base 和 PTS 计算）
 * @bitrate：目标码率（bps），如 4000000 = 4Mbps
 * @gop：关键帧间隔（GOP size）
 * 返回：encoder_t*（失败返回 NULL）
 */
encoder_t* encoder_open(uint32_t w, uint32_t h, uint32_t fps,
                        uint32_t bitrate, uint32_t gop) {
    encoder_t* e = calloc(1, sizeof(encoder_t));
    if (!e) return NULL;
    e->width = w; e->height = h;

    /*
     * 查找 libx264 编码器
     * libx264 是软件 H.264 编码器，广泛使用、稳定高效
     * 未来可替换为 h264_rkmpp（Rockchip 硬件编码器）以降低 CPU 占用
     */
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) { LOG_ERROR("libx264 not found"); encoder_close(e); return NULL; }

    /* 分配编码器上下文 */
    e->ctx = avcodec_alloc_context3(codec);
    if (!e->ctx) { LOG_ERROR("avcodec_alloc_context3 failed"); encoder_close(e); return NULL; }

    /*
     * 设置编码参数
     *
     * time_base = {1, fps}：时间基为 1/fps 秒
     *   例如 fps=30 → time_base={1,30} → 每帧 PTS 递增 1，代表 1/30 秒
     *
     * pix_fmt = AV_PIX_FMT_YUV420P：x264 编码器要求的输入格式
     *   NV12 通过 sws_scale 转换为 YUV420P
     *
     * bit_rate：码率（bps）
     *   4Mbps = 4,000,000 bps，对于 1080P@30fps 是常用值
     *   码率越高画质越好，但推流带宽需求也越高
     *
     * gop_size：关键帧间隔
     *   gop_size=60 @ 30fps = 每 2 秒一个 I 帧
     *   I 帧（关键帧）是完整的图像帧，P/B 帧只存储差异
     *   RTMP 播放器需要 I 帧来启动解码
     */
    e->ctx->width     = w;
    e->ctx->height    = h;
    e->ctx->time_base = (AVRational){1, (int)fps};
    e->ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    e->ctx->bit_rate  = bitrate;
    e->ctx->gop_size  = gop;
    e->ctx->max_b_frames = 0;

    /* 打开编码器 */
    int ret = avcodec_open2(e->ctx, codec, NULL);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_ERROR("avcodec_open2 failed: %s", err);
        encoder_close(e); return NULL;
    }

    /*
     * 分配 AVFrame 缓冲
     * av_frame_get_buffer() 根据编码器上下文的宽高和像素格式自动分配
     * YUV420P 三平面：Y 平面（w×h）+ U 平面（w/2×h/2）+ V 平面（w/2×h/2）
     */
    e->frame = av_frame_alloc();
    e->frame->format = e->ctx->pix_fmt;
    e->frame->width  = w; e->frame->height = h;
    av_frame_get_buffer(e->frame, 0);

    /*
     * 创建像素格式转换器：NV12 → YUV420P
     *
     * sws_getContext 参数：
     *   srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags
     *
     * SWS_FAST_BILINEAR：快速双线性插值
     *   虽然 NV12→YUV420P 只是平面重排（不需要真正的缩放），
     *   但 sws_scale 对平面重排有优化路径，实际效率很高
     */
    e->sws = sws_getContext(w, h, AV_PIX_FMT_NV12,
                            w, h, AV_PIX_FMT_YUV420P,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL);

    /* 分配 AVPacket（编码器输出容器） */
    e->pkt = av_packet_alloc();

    LOG_INFO("encoder: libx264 %ux%u@%ufps %ukbps", w, h, fps, bitrate / 1000);
    return e;
}

/*
 * 关闭编码器并释放所有资源
 * 顺序：释放编码器上下文 → 释放 AVFrame → 释放 AVPacket → 释放格式转换器
 */
void encoder_close(encoder_t* e) {
    if (!e) return;
    if (e->ctx)   avcodec_free_context(&e->ctx);
    if (e->frame) av_frame_free(&e->frame);
    if (e->pkt)   av_packet_free(&e->pkt);
    if (e->sws)   sws_freeContext(e->sws);
    free(e);
}

/*
 * 编码一帧 NV12 视频数据
 *
 * 编码流程（完整的 send/receive 模型）：
 *   1. 将 NV12 数据通过 sws_scale 转换为 YUV420P
 *   2. avcodec_send_frame()：将 YUV420P 帧送入编码器（可能被缓冲）
 *   3. 循环调用 avcodec_receive_packet()：取出所有已完成的编码包
 *      - AVERROR(EAGAIN)：编码器需要更多帧才能输出（正常，继续等待）
 *      - AVERROR_EOF：编码器已刷新完毕（正常，流结束）
 *      - 正常返回：得到一个编码好的 H.264 包，通过回调发送
 *
 * 注意：编码器内部有延迟（帧重排序、B 帧引用等），
 *       可能送 1 帧出 0 个包，也可能出多个包（如 I 帧切割为多个 slice）
 *
 * @e：编码器实例
 * @nv12：输入 NV12 数据指针
 * @pts：显示时间戳（Presentation Timestamp），用于播放同步
 * 返回：0 成功，-1 失败
 */
int encoder_encode(encoder_t* e, const uint8_t* nv12, int64_t pts) {
    if (!e || !e->ctx || !nv12) return -1;

    /*
     * 步骤 1：NV12 → YUV420P 格式转换
     *
     * NV12 内存布局：
     *   Y 平面：w×h 字节，从 nv12 开始
     *   UV 平面：w×h/2 字节，从 nv12 + w*h 开始（UV 交错）
     *
     * sws_scale 通过 src[2] 和 strides[2] 指定两个平面的位置和步长
     * 转换结果写入 e->frame->data（YUV420P 三平面）
     */
    const uint8_t* src[2] = { nv12, nv12 + e->width * e->height };
    int strides[2] = { (int)e->width, (int)e->width };
    sws_scale(e->sws, src, strides, 0, e->height,
              e->frame->data, e->frame->linesize);

    /*
     * 设置 PTS（Presentation Timestamp）
     * 使用帧序号作为 PTS，简单有效
     * 如果外部传入了有效 PTS（>=0），优先使用外部值
     */
    if (pts < 0) pts = e->frame_n;
    e->frame->pts = pts;
    e->frame_n++;

    /*
     * 步骤 2：将帧送入编码器
     * avcodec_send_frame() 会引用帧数据（不拷贝），
     * 所以在函数返回后不要立即修改或释放帧数据
     */
    int ret = avcodec_send_frame(e->ctx, e->frame);
    if (ret < 0) return -1;

    /*
     * 步骤 3：取出所有已完成的编码包
     *
     * 编码器可能缓冲了多帧才输出，所以用 while 循环取完
     * av_packet_unref() 释放前一个包的数据，避免内存泄漏
     *
     * 输出包通过回调 cb 传递给 RTMP 推流器：
     *   - data：编码后的 H.264 数据（NAL unit）
     *   - size：数据字节数
     *   - pts：显示时间戳
     *   - keyframe：是否为关键帧（I 帧，用于 RTMP 播放器快速启播）
     */
    while (ret >= 0) {
        av_packet_unref(e->pkt);  /* 清理上一个包的数据 */
        ret = avcodec_receive_packet(e->ctx, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;  /* 正常：暂无输出/已结束 */
        if (ret < 0) return -1;
        if (e->cb)
            e->cb(e->cb_data, e->pkt->data, e->pkt->size, e->pkt->pts,
                  (e->pkt->flags & AV_PKT_FLAG_KEY));  /* AV_PKT_FLAG_KEY = 关键帧标志 */
    }
    return 0;
}

/*
 * 刷新编码器缓冲区
 *
 * 在停止编码时调用，确保编码器内部所有缓冲的帧都被编码并输出
 * 调用 avcodec_send_frame(ctx, NULL) 进入排空（drain）模式
 * 然后循环取出剩余的编码包，直到收到 AVERROR_EOF
 */
int encoder_flush(encoder_t* e) {
    if (!e || !e->ctx) return -1;
    avcodec_send_frame(e->ctx, NULL);  /* 发送 NULL 帧 → 进入排空模式 */
    for (;;) {
        av_packet_unref(e->pkt);
        int ret = avcodec_receive_packet(e->ctx, e->pkt);
        if (ret == AVERROR_EOF) break;  /* 排空完毕 */
        if (ret < 0) return -1;
        if (e->cb)
            e->cb(e->cb_data, e->pkt->data, e->pkt->size, e->pkt->pts,
                  (e->pkt->flags & AV_PKT_FLAG_KEY));
    }
    return 0;
}

/*
 * 设置编码输出回调函数
 * 每个编码完成的 H.264 包都会通过回调发送给 RTMP 推流器
 *
 * @cb：回调函数指针
 * @userdata：回调时透传的用户数据（通常是 rtmp_t* 指针）
 */
void encoder_set_callback(encoder_t* e, enc_packet_cb cb, void* userdata) {
    if (e) { e->cb = cb; e->cb_data = userdata; }
}

/* 检查编码器是否已就绪 */
int encoder_ready(encoder_t* e) { return e && e->ctx ? 1 : 0; }
