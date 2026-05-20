/*
 * ==========================================================================
 * v4l2.h — V4L2 视频采集模块头文件
 * ==========================================================================
 *
 * **V4L2（Video4Linux2）** 是 Linux 内核的视频设备驱动框架
 * 本项目通过 V4L2 驱动 IMX415 MIPI 摄像头，采集 NV12 格式的视频流
 *
 * **采集模式**：
 *   - mmap + DMA Buffer：零拷贝内存映射，ISP 直接写入 DMA 缓冲区
 *   - epoll：异步 I/O 多路复用，等待帧就绪通知
 *
 * **典型使用流程**：
 *   v4l2_open() → v4l2_start() → [循环: v4l2_capture()] → v4l2_stop() → v4l2_close()
 */

#ifndef V4L2_H
#define V4L2_H

#include "types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄：隐藏 V4L2 内部实现细节 */
typedef struct v4l2_cap_s v4l2_cap_t;

/*
 * 打开并配置 V4L2 设备
 * @device：设备节点路径（如 "/dev/video0"）
 * @width, height：期望的分辨率
 * @fps：期望的帧率
 * @pixfmt：像素格式字符串（"NV12"/"MJPEG"/"YUYV"）
 * @buf_count：DMA 缓冲区数量（建议 4~8）
 * 返回：v4l2_cap_t*（不透明句柄），失败返回 NULL
 */
v4l2_cap_t* v4l2_open(const char* device, uint32_t width, uint32_t height,
                       uint32_t fps, const char* pixfmt, uint32_t buf_count);

/* 启动视频流（QBUF 所有缓冲区 + STREAMON + 创建 epoll） */
int         v4l2_start(v4l2_cap_t* cap);

/* 停止视频流（STREAMOFF + 关闭 epoll） */
void        v4l2_stop(v4l2_cap_t* cap);

/* 关闭设备并释放所有资源（munmap + close + free） */
void        v4l2_close(v4l2_cap_t* cap);

/*
 * 采集一帧视频数据
 * 使用 epoll 等待帧就绪，DQBUF 取出缓冲区，memcpy 拷贝数据
 * 返回 frame_t*（调用者负责通过 frame_free 释放），超时/错误返回 NULL
 */
frame_t*    v4l2_capture(v4l2_cap_t* cap);

/* 获取 V4L2 设备文件描述符 */
int         v4l2_get_fd(v4l2_cap_t* cap);

/* 检查采集是否正在运行 */
bool        v4l2_is_running(v4l2_cap_t* cap);

#ifdef __cplusplus
}
#endif

#endif /* V4L2_H */
