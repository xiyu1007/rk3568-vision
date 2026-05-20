/*
 * ==========================================================================
 * rtmp.c — RTMP 推流模块（基于 FFmpeg libavformat）
 * ==========================================================================
 *
 * **功能**：将 H.264 编码数据通过 RTMP 协议推送到流媒体服务器
 *
 * **RTMP 协议简介**：
 *   RTMP（Real-Time Messaging Protocol）是 Adobe 开发的实时流媒体协议
 *   基于 TCP 长连接，支持低延迟（通常 1-3 秒）视频传输
 *   广泛应用于直播推流场景（推流到 nginx-rtmp、SRS、YouTube、B站 等）
 *
 * **推流流程**：
 *   1. avformat_alloc_output_context2()：创建 FLV 格式的输出上下文
 *   2. avformat_new_stream()：创建视频流
 *   3. 设置视频流参数：H.264 编码、分辨率、帧率、时间基
 *   4. avio_open()：打开 RTMP URL（建立 TCP 连接 + RTMP 握手）
 *   5. avformat_write_header()：写入 FLV 文件头和 RTMP 元数据
 *   6. 循环调用 av_interleaved_write_frame()：推送 H.264 编码包
 *   7. 结束时 av_write_trailer() + avio_close()：关闭连接
 *
 * **FLV 容器格式**：
 *   RTMP 使用 FLV（Flash Video）作为封装格式
 *   FLV tag 包含：视频数据 + 时间戳 + 关键帧标志
 *   av_interleaved_write_frame 自动处理封装和交错
 */

#include "rtmp.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>   /* AVFormatContext, avio_open, av_interleaved_write_frame */
#include <libavcodec/avcodec.h>     /* AVCodecParameters, AVPacket                           */

/*
 * RTMP 推流器内部状态
 */
struct rtmp_s {
    AVFormatContext* ctx;          /* FFmpeg 格式上下文（FLV 格式） */
    AVStream*        vs;           /* 视频流指针                      */
    char             url[256];     /* RTMP 推流地址备份              */
    uint32_t         width, height, fps;  /* 视频参数                */
    int              connected;    /* 连接状态（1=已连接, 0=已断开） */
};

/*
 * 打开 RTMP 推流连接
 *
 * 完整初始化流程：
 *   1. 创建 FLV 格式的输出上下文
 *   2. 创建视频流并设置编码参数
 *   3. 打开 RTMP URL（avio_open）
 *   4. 写入 FLV 文件头（avformat_write_header）
 *
 * 为什么用 FLV 封装格式？
 *   - FLV 是 RTMP 协议的标准封装格式
 *   - 简单高效，tag 头仅 11 字节
 *   - 适合实时流传输（不需要 seek 操作）
 *
 * @url：RTMP 服务器地址，格式如 "rtmp://192.168.1.100/live/stream"
 * @width, height：视频分辨率
 * @fps：帧率
 * @bitrate：码率（用于 RTMP 元数据，实际编码码率由编码器控制）
 * 返回：rtmp_t*（失败返回 NULL）
 */
rtmp_t* rtmp_open(const char* url, uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate) {
    rtmp_t* r = calloc(1, sizeof(rtmp_t));
    if (!r) return NULL;
    strncpy(r->url, url, 255);
    r->width = width; r->height = height; r->fps = fps;

    /*
     * 创建 FLV 格式的输出上下文
     * "flv" 指定封装格式，FFmpeg 自动匹配 RTMP 协议
     * 如果 url 为 NULL，则创建纯内存输出（无网络连接）
     */
    int ret = avformat_alloc_output_context2(&r->ctx, NULL, "flv", url);
    if (ret < 0 || !r->ctx) {
        LOG_ERROR("rtmp: alloc output context failed");
        free(r); return NULL;
    }

    /*
     * 创建视频流
     * avformat_new_stream 创建一条新的媒体流，返回 AVStream 指针
     * 流 ID 自动分配（通常从 0 开始）
     */
    r->vs = avformat_new_stream(r->ctx, NULL);
    r->vs->id = r->ctx->nb_streams - 1;

    /*
     * 设置视频流编码参数（codecpar）
     *
     * time_base = {1, fps}：时间基，与编码器保持一致
     *   例如 fps=30 → {1,30} → 每帧 PTS 递增 1，代表 1/30 秒
     *   RTMP 客户端根据 PTS 和 time_base 计算实际播放时间
     *
     * codec_type = AVMEDIA_TYPE_VIDEO：视频流类型
     * codec_id = AV_CODEC_ID_H264：H.264 编码
     * width, height：视频分辨率（用于播放器初始化显示窗口）
     */
    r->vs->time_base = (AVRational){1, (int)fps};
    r->vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    r->vs->codecpar->codec_id   = AV_CODEC_ID_H264;
    r->vs->codecpar->width      = width;
    r->vs->codecpar->height     = height;

    /*
     * 打开 RTMP 连接
     * avio_open 建立 TCP 连接并完成 RTMP 握手
     * AVIO_FLAG_WRITE：只写模式（推流不需要读）
     * 成功后将 ctx->pb 指向建立的 I/O 上下文
     */
    ret = avio_open(&r->ctx->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0) {
        LOG_ERROR("rtmp: avio_open failed for %s", url);
        avformat_free_context(r->ctx);
        free(r); return NULL;
    }

    /*
     * 写入 FLV 文件头和 RTMP 元数据
     * 包括：
     *   - FLV 签名（"FLV\x01"）
     *   - 音视频流信息（分辨率、帧率、编码格式）
     *   - onMetaData（供播放器解析）
     */
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

/*
 * 关闭 RTMP 推流连接
 *
 * 关闭流程：
 *   1. 标记 disconnected（防止后续 push 调用）
 *   2. av_write_trailer()：写入 FLV 文件尾（更新 duration 等信息）
 *   3. avio_closep()：关闭 TCP 连接
 *   4. avformat_free_context()：释放格式上下文
 */
void rtmp_close(rtmp_t* r) {
    if (!r) return;
    r->connected = 0;
    if (r->ctx) {
        if (r->ctx->pb) {
            av_write_trailer(r->ctx);   /* 写入 FLV 尾部数据 */
            avio_closep(&r->ctx->pb);   /* 关闭网络连接 */
        }
        avformat_free_context(r->ctx);   /* 释放整个格式上下文 */
    }
    free(r);
}

/*
 * 推送一帧 H.264 编码数据到 RTMP 服务器
 *
 * 每次从编码器收到一个 H.264 包时调用此函数
 * 包中包含一个或多个 NAL unit（网络抽象层单元）
 *
 * 关键帧标志的作用：
 *   - RTMP 服务器和播放器需要 I 帧来初始化解码器
 *   - I 帧包含完整的图像数据，P/B 帧只包含差异
 *   - 播放器在收到 I 帧之前无法显示画面
 *   - GOP=60 意味着每 60 帧（2 秒 @30fps）有一个 I 帧
 *
 * @data：H.264 编码数据
 * @size：数据字节数
 * @pts：显示时间戳（来自编码器）
 * @keyframe：是否为关键帧（I 帧）
 * 返回：0 成功，-1 失败（连接断开或写错误）
 */
int rtmp_push_video(rtmp_t* r, const uint8_t* data, size_t size,
                     int64_t pts, int keyframe) {
    if (!r || !r->connected || !r->ctx || !r->vs) return -1;

    /*
     * 创建 AVPacket（编码数据包）
     *
     * 注意：data 指针直接指向传入的编码数据，不进行拷贝
     * 这意味着调用者在 av_interleaved_write_frame 返回前不能释放 data
     *
     * stream_index：指定该包属于哪个流（视频流）
     * pts == dts：H.264 推流中通常 PTS 等于 DTS（无 B 帧时）
     * AV_PKT_FLAG_KEY：关键帧标志（播放器据此判断可否开始解码）
     */
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return -1;
    pkt->data         = (uint8_t*)data;      /* 直接引用，不拷贝（性能优化） */
    pkt->size         = (int)size;
    pkt->stream_index = r->vs->index;
    pkt->pts = pts;
    pkt->dts = pts;                          /* H.264 无 B 帧时 PTS == DTS */
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

    /*
     * 写入帧数据到 RTMP 流
     *
     * av_interleaved_write_frame 负责：
     *   1. 封装为 FLV tag（添加 tag 头：类型、数据大小、时间戳）
     *   2. 交错写入（音视频流混合时保证顺序，纯视频时简单顺序写入）
     *   3. 通过 TCP socket 发送到 RTMP 服务器
     *
     * 如果返回错误（如网络断开），标记连接为断开状态
     */
    int ret = av_interleaved_write_frame(r->ctx, pkt);
    av_packet_free(&pkt);                    /* 释放 packet 结构体（不释放 data） */

    if (ret < 0) {
        LOG_ERROR("rtmp: write frame failed");
        r->connected = 0;                    /* 标记连接断开，后续调用将直接返回 */
        return -1;
    }
    return 0;
}

/* 检查 RTMP 连接状态 */
int rtmp_is_connected(rtmp_t* r) { return r && r->connected; }
