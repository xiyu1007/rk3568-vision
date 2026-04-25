#ifndef __V4L2_CAPTURE_H__
#define __V4L2_CAPTURE_H__

#include "common.h"

#include <time.h>
#include <linux/videodev2.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>  // 直接包含标准头文件，不需要自己定义

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LOGI
    #define V4L2_LOGI LOGI
#else
    #define V4L2_LOGI ((void)0 )
#endif

#ifdef LOGE
    #define V4L2_LOGE LOGE
#else
    #define V4L2_LOGE ((void)0 )
#endif

#define CLEAR(x) memset(&(x), 0, sizeof(x))  // 清空结构体

/* mmap buffer */
// 描述的是内存资源本身（容量/地址）
typedef struct {
    void *start[VIDEO_MAX_PLANES];     // 映射起始地址
    size_t  length[VIDEO_MAX_PLANES];     // mmap长度（固定）buffer容量（最大） QUERYBUF 后
    size_t  bytesused[VIDEO_MAX_PLANES];  // 实际数据长度（DQBUF后）
} v4l2_buffer_t;                        // MMAP缓冲区结构

/* context */
typedef struct {
    int fd;                             // 设备文件描述符
    const char *dev;                    // 设备路径

    int width, height, fps;             // 宽、高、帧率
    uint32_t pixfmt;                    // 像素格式
    uint32_t buf_type;                  // 缓冲区类型

    v4l2_buffer_t *buffers;             // 缓冲区数组
    int n_buffers;                      // 缓冲区数量
    int n_planes;                       // 平面数量
    int stride[VIDEO_MAX_PLANES];      // 不同 plane 的 stride 可以不同
    int sizeimage[VIDEO_MAX_PLANES];   // 
} v4l2_ctx_t;                           // V4L2上下文结构


// ========================================================
typedef int (*v4l2_frame_cb)(
    v4l2_ctx_t *ctx,
    const v4l2_buffer_t *frame,
    void *user
);

/* API */
int v4l2_init(v4l2_ctx_t *ctx);         // 初始化V4L2设备
int v4l2_start(v4l2_ctx_t *ctx);        // 开始视频采集

/* read */
// 出队缓冲区（获取一帧）
// 回调
// 入队缓冲区（归还缓冲区）
int v4l2_read(v4l2_ctx_t *ctx, v4l2_frame_cb cb, void *user);

void v4l2_stop(v4l2_ctx_t *ctx);        // 停止视频采集

#ifdef __cplusplus
}
#endif

#endif  // __V4L2_CAPTURE_H__