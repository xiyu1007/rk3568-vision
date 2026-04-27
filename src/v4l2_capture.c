#include "v4l2_capture.h"      // V4L2采集头文件，包含结构体和函数声明

#include <stdio.h>              // 标准输入输出，用于调试打印
#include <stdlib.h>             // 标准库，用于calloc/free等内存操作
#include <fcntl.h>              // 文件控制，提供open函数
#include <unistd.h>             // POSIX系统调用，提供close/munmap等
#include <errno.h>              // 错误码，用于errno变量
#include <sys/ioctl.h>          // ioctl系统调用，控制设备
#include <sys/mman.h>           // 内存映射，提供mmap/munmap

static inline int xioctl(int fd, int req, void *arg)
{
    int r;
    // 当进程收到信号（如 SIGINT、SIGALRM）时，正在阻塞的系统调用会返回 -1，并设置 errno = EINTR
    // 这不是真正的错误，而是系统允许信号处理的一种机制
    while ((r = ioctl(fd, req, arg)) == -1 && errno == EINTR);  // 被信号中断则重试
    return r;
}

/* 打开设备 */
static int open_device(v4l2_ctx_t *ctx)
{
    // 非阻塞方式打开设备，避免DQBUFFER时阻塞
    ctx->fd = open(ctx->dev, O_RDWR | O_NONBLOCK);
    // 整个逗号表达式的值是最后一个表达式的值
    return (ctx->fd < 0) ? (V4L2_LOGE("open %s err=%d", ctx->dev, errno), -1) : (V4L2_LOGI("open_device successful"), 0);
}

/* 查询设备能力 + 选择缓冲区类型 */
static int query_cap(v4l2_ctx_t *ctx)
{
    struct v4l2_capability cap;  // 设备能力结构体
    CLEAR(cap);                   // 清零

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0)  // 查询能力
        return V4L2_LOGE("query_cap err=%d", errno), -1;

    uint32_t c = cap.capabilities;                    // 获取设备能力标志
    if (c & V4L2_CAP_DEVICE_CAPS) c = cap.device_caps; // 使用设备具体能力

    // 打印设备能力标志（十六进制）
    V4L2_LOGI("query_cap Capabilities: 0x%08X", c);

    // 选择缓冲区类型：优先多平面，其次单平面
    if (c & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        ctx->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  // 多平面格式
    else if (c & V4L2_CAP_VIDEO_CAPTURE)
        ctx->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;         // 单平面格式
    else
        return V4L2_LOGE("no capture"), -1;                  // 不支持采集

    V4L2_LOGI("query_cap buf_type: %s", c & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? "MPLANE" : "SINGLE PLANE");

    // 检查是否支持流式IO
    return (c & V4L2_CAP_STREAMING) ? (V4L2_LOGI("query_cap Streaming I/O supported"), 0) : (V4L2_LOGE("no stream"), -1);

}

/* 设置视频格式 */
static int set_format(v4l2_ctx_t *ctx)
{
    struct v4l2_format fmt;   // 格式结构体
    CLEAR(fmt);                // 清零

    fmt.type = ctx->buf_type;  // 设置缓冲区类型

    // 配置格式参数（使用多平面结构体pix_mp）
    fmt.fmt.pix_mp.width = ctx->width;           // 图像宽度
    fmt.fmt.pix_mp.height = ctx->height;         // 图像高度
    fmt.fmt.pix_mp.pixelformat = ctx->pixfmt;    // 像素格式(YUYV/MJPEG等)
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;       // 场序，ANY表示不关心

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0){
        return (V4L2_LOGE("set_format FMT err=%d", errno), -1);
    }

    ctx->n_planes = fmt.fmt.pix_mp.num_planes ? fmt.fmt.pix_mp.num_planes : 1;
    V4L2_LOGI("set_format n_planes=%d", ctx->n_planes);

    if (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        for (int i = 0; i < ctx->n_planes; i++) {
            ctx->stride[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
            ctx->sizeimage[i] = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
            V4L2_LOGI("set_format plane[%d]: stride=%d sizeimage=%d", i, ctx->stride[i], ctx->sizeimage[i]);
        }
    }

    // 打印设置后的值（驱动可能修改了）
    V4L2_LOGI("set_format FMT:  %dx%d, format=%c%c%c%c",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat & 0xFF,
           (fmt.fmt.pix_mp.pixelformat >> 8) & 0xFF,
           (fmt.fmt.pix_mp.pixelformat >> 16) & 0xFF,
           (fmt.fmt.pix_mp.pixelformat >> 24) & 0xFF);
    
    // 更新 ctx 为实际设置的值
    ctx->width = fmt.fmt.pix_mp.width;
    ctx->height = fmt.fmt.pix_mp.height;
    ctx->pixfmt = fmt.fmt.pix_mp.pixelformat;
    return 0;
}

/* 设置帧率 */
static int set_fps(v4l2_ctx_t *ctx)
{
    struct v4l2_streamparm parm;
    CLEAR(parm);

    parm.type = ctx->buf_type;
    if (xioctl(ctx->fd, VIDIOC_G_PARM, &parm) < 0){
        if (errno == ENOTTY)
            return 0;
        return V4L2_LOGE("set_fps G_PARM err=%d", errno), -1;
    }

    if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        V4L2_LOGI("set_fps fps not supported");
        return 0;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = ctx->fps;

    if (xioctl(ctx->fd, VIDIOC_S_PARM, &parm) < 0)
        return V4L2_LOGE("set_fps S_PARM err=%d", errno), -1;

    ctx->fps = parm.parm.capture.timeperframe.denominator;

    V4L2_LOGI("set_fps FPS=%d", ctx->fps);
    return 0;
}

/* 初始化MMAP内存映射 */
static int init_mmap(v4l2_ctx_t *ctx)
{
    struct v4l2_requestbuffers req;  // 请求缓冲区结构体
    CLEAR(req);

    req.count = ctx->n_buffers ? ctx->n_buffers : 4;    // 请求4个缓冲区
    req.type = ctx->buf_type;                           // 缓冲区类型
    req.memory = V4L2_MEMORY_MMAP;                      // 使用MMAP内存类型

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0)      // 请求缓冲区
        return V4L2_LOGE("init_mmap REQBUFS err=%d", errno), -1;

    V4L2_LOGI("init_mmap REQBUFS num=%d", req.count);

    // 分配缓冲区数组并保存实际分配的数量
    ctx->buffers = (v4l2_buffer_t *)calloc(req.count, sizeof(v4l2_buffer_t));
    ctx->n_buffers = req.count;

    // 遍历每个缓冲区进行映射
    for (int i = 0; i < ctx->n_buffers; i++) {

        struct v4l2_buffer buf;                    // 缓冲区结构
        struct v4l2_plane planes[VIDEO_MAX_PLANES]; // 多平面数组

        CLEAR(buf); CLEAR(planes);

        buf.type = ctx->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;                              // 缓冲区索引

        // 如果是多平面格式，设置planes数组
        if (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;                  // 注意这里指针指向了planes，后续planes会因VIDIOC_QUERYBUF变化
            buf.length = VIDEO_MAX_PLANES;          // 最大平面数
        }

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0)  // 查询缓冲区信息
            return V4L2_LOGE("init_mmap QUERYBUF err=%d", errno), -1;

        // 确定平面数量：多平面用buf.length，单平面为1
        // 在 MPLANE 模式下，buf.length 表示 plane 数量, 当前 buffer 有多少个 plane
        // 单平面 buf.length 表示 buffer 大小（字节）, 没有 plane
        int mplane = (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        int cnt = mplane ? buf.length : 1;

        // 当前 buffer 有多少个 plane， queue_all中使用
        // 与 set_format 不匹配
        if (ctx->n_planes != cnt) {
            V4L2_LOGE("init_mmap plane mismatch: ctx=%d buf=%d", ctx->n_planes, cnt);
            return -1;
        }

        // 打印缓冲区信息
        V4L2_LOGI("init_mmap Buffer[%d]: n_planes=%d, type=%s", i, cnt, mplane ? "MPLANE" : "SINGLE");

        // 遍历每个平面进行内存映射
        for (int p = 0; p < cnt; p++) {
            // 获取缓冲区长度：多平面从planes获取，单平面从buf获取
            // mmap长度（固定）,最大映射长度
            ctx->buffers[i].length[p] =
                mplane ? planes[p].length : buf.length; 

            // mmap内存映射，将内核空间映射到用户空间
            ctx->buffers[i].start[p] =
                mmap(NULL,
                     ctx->buffers[i].length[p],          // 映射长度
                     PROT_READ | PROT_WRITE,              // 读写权限
                     MAP_SHARED,                          // 共享映射
                     ctx->fd,
                     mplane ? planes[p].m.mem_offset : buf.m.offset);  // 偏移量

            if (ctx->buffers[i].start[p] == MAP_FAILED)   // 映射失败
                return V4L2_LOGE("init_mmap mmap i=%d p=%d err=%d", i, p, errno), -1;

            // 打印每个平面的映射信息
            V4L2_LOGI("init_mmap Plane[%d]: size=%zu bytes, offset=%ju, start=%p",p, ctx->buffers[i].length[p],
                      (uintmax_t)(mplane ? planes[p].m.mem_offset : buf.m.offset),
                      ctx->buffers[i].start[p]);
        }
    }

    V4L2_LOGI("init_mmap MMAP successful, n_buffers=%d", ctx->n_buffers);
    
    return 0;
}

/* 将所有缓冲区入队 */
static int queue_all(v4l2_ctx_t *ctx)
{
    struct v4l2_buffer buf;                    // 缓冲区结构
    struct v4l2_plane planes[VIDEO_MAX_PLANES]; // 多平面数组

    for (int i = 0; i < ctx->n_buffers; i++) {

        CLEAR(buf); CLEAR(planes);

        buf.type = ctx->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;                          // 缓冲区索引

        // 多平面格式需要设置planes和长度
        if (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = ctx->n_planes;  // 平面数量

            // 设置每个平面的长度
            for (uint32_t p = 0; p < buf.length; p++)
                planes[p].length = ctx->buffers[i].length[p];
        }

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0)  // 入队缓冲区
            return V4L2_LOGE("queue_all QBUF err=%d", errno), -1;

        V4L2_LOGI("queue_all QUEUE buffer.index=%d", i);
    }

    V4L2_LOGI("queue_all QUEUE successful");
    return 0;
}


/* 初始化V4L2设备（主函数） */
int v4l2_init(v4l2_ctx_t *ctx)
{
    // 依次执行各个初始化步骤，任何一步失败则返回-1
    return open_device(ctx) ||   // 1.打开设备
           query_cap(ctx) ||     // 2.查询能力
           set_format(ctx) ||    // 3.设置格式
           set_fps(ctx) ||       // 4.设置帧率
           init_mmap(ctx) ||     // 5.初始化MMAP
           queue_all(ctx);       // 6.所有缓冲区入队
}

/* 启动视频流 */
int v4l2_start(v4l2_ctx_t *ctx)
{
    int type = ctx->buf_type;  // 缓冲区类型
    // 检查是否支持流式IO
    return xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0   // 启动流
        ? (V4L2_LOGE("v4l2_start STREAMON err=%d", errno), -1) 
        : (V4L2_LOGI("v4l2_start ...") , 0);
}

    // return (c & V4L2_CAP_STREAMING) ? 0 : (V4L2_LOGE("no stream"), -1);

int v4l2_read(v4l2_ctx_t *ctx, v4l2_frame_cb cb, void *user)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    v4l2_buffer_t frame; // 视图实际数据指针

    CLEAR(buf);
    CLEAR(planes);
    CLEAR(frame);

    buf.type = ctx->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
    }

    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0)
        return (errno == EAGAIN) ? 1 : (V4L2_LOGE("v4l2_read DQBUF err=%d", errno), -1);

    int idx = buf.index;

    int cnt = (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            ? ctx->n_planes : 1;

    for (int i = 0; i < cnt; i++) {
        frame.start[i] = (uint8_t *)ctx->buffers[idx].start[i];
        frame.bytesused[i]  = (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                  ? buf.m.planes[i].bytesused // 多平面
                  : buf.bytesused ;              // 单品面
    }

    /* 用户处理（核心扩展点） */
    if (cb) cb(ctx, &frame, user);

    // 重新入队前，需要重新设置 length（因为 DQBUF 后内核可能修改了）
    // if (ctx->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    //     for (int i = 0; i < ctx->n_planes; i++) {
    //         buf.m.planes[i].length = ctx->buffers[idx].length[i];
    //     }
    // }

    // VIDIOC_DQBUF已经修改 planes.length
    /* 立即归还，重新入队， buffer */
    return xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0 ? (V4L2_LOGE("v4l2_read QBUF err=%d", errno), -1) : 0;

}

/* 停止视频流并释放资源 */
void v4l2_stop(v4l2_ctx_t *ctx)
{
    int type = ctx->buf_type;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);  // 停止流

    if (!ctx->buffers) return;  // 没有缓冲区则直接返回

    // 解映射所有缓冲区
    for (int i = 0; i < ctx->n_buffers; i++)
        for (int p = 0; p < ctx->n_planes; p++)
            munmap(ctx->buffers[i].start[p],    // 起始地址
                   ctx->buffers[i].length[p]);  // 映射长度

    free(ctx->buffers);   // 释放缓冲区数组
    ctx->buffers = NULL;

    close(ctx->fd);       // 关闭设备
    ctx->fd = -1;
    V4L2_LOGI("v4l2_stop \n");
}