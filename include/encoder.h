/*
 * ==========================================================================
 * encoder.h — FFmpeg H.264 编码器头文件
 * ==========================================================================
 *
 * **编码回调机制**：
 *   enc_packet_cb 是编码完成后的回调函数
 *   每产生一个 H.264 NAL 包就调用一次
 *   pipeline.c 将此回调连接到 rtmp_callback，实现编码→推流的数据流
 *
 * **编码流程**：
 *   encoder_open() → [循环: encoder_encode()] → encoder_flush() → encoder_close()
 */

#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 编码包回调函数类型
 * @userdata：用户数据（通常是 rtmp_t* 推流器指针）
 * @data：H.264 编码数据（NAL unit）
 * @size：数据字节数
 * @pts：显示时间戳（Presentation Timestamp）
 * @keyframe：是否为关键帧（I 帧，用于 RTMP 播放器启播）
 */
typedef void (*enc_packet_cb)(void* userdata, const uint8_t* data, size_t size,
                               int64_t pts, int keyframe);

/* 不透明句柄 */
typedef struct encoder_s encoder_t;

/* 创建编码器实例
 * @width, height：视频分辨率
 * @fps：帧率
 * @bitrate：目标码率（bps）
 * @gop_size：关键帧间隔（帧数）
 */
encoder_t* encoder_open(uint32_t width, uint32_t height, uint32_t fps,
                        uint32_t bitrate, uint32_t gop_size);

/* 销毁编码器 */
void       encoder_close(encoder_t* e);

/* 编码一帧 NV12 数据 */
int        encoder_encode(encoder_t* e, const uint8_t* nv12, int64_t pts);

/* 刷新编码器缓冲区（停止时调用，排空所有残余帧） */
int        encoder_flush(encoder_t* e);

/* 设置编码输出回调（每个编码包通过回调发送） */
void       encoder_set_callback(encoder_t* e, enc_packet_cb cb, void* userdata);

/* 检查编码器是否就绪 */
int        encoder_ready(encoder_t* e);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
