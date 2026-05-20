/*
 * ==========================================================================
 * rtmp.h — RTMP 推流模块头文件
 * ==========================================================================
 *
 * **模块职责**：将 H.264 编码数据推送到 RTMP 流媒体服务器
 *
 * **RTMP 协议栈**：
 *   应用层：RTMP（Adobe 流媒体协议）
 *   传输层：TCP（可靠传输，低延迟）
 *   封装格式：FLV（Flash Video）
 *
 * **使用流程**：
 *   rtmp_open() → [循环: rtmp_push_video()] → rtmp_close()
 */

#ifndef RTMP_H
#define RTMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄 */
typedef struct rtmp_s rtmp_t;

/*
 * 创建 RTMP 推流连接
 * @url：RTMP 服务器地址（如 "rtmp://192.168.1.100/live/stream"）
 * @width, height：视频分辨率
 * @fps：帧率
 * @bitrate：码率（用于 RTMP 元数据）
 */
rtmp_t* rtmp_open(const char* url, uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate);

/* 关闭推流连接 */
void    rtmp_close(rtmp_t* r);

/*
 * 推送一帧 H.264 编码视频数据
 * @data：H.264 编码数据
 * @size：数据字节数
 * @pts：显示时间戳
 * @keyframe：是否为关键帧（I 帧）
 */
int     rtmp_push_video(rtmp_t* r, const uint8_t* data, size_t size,
                          int64_t pts, int keyframe);

/* 检查连接状态 */
int     rtmp_is_connected(rtmp_t* r);

#ifdef __cplusplus
}
#endif
#endif /* RTMP_H */
