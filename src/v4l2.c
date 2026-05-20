/*
 * ==========================================================================
 * v4l2.c — V4L2 视频采集模块（Video4Linux2）
 * ==========================================================================
 *
 * **功能**：通过 Linux V4L2 框架从 IMX415 MIPI 摄像头采集 NV12 视频流
 *
 * **零拷贝原理（mmap/DMA Buffer）**
 *   1. 驱动在内核空间分配 DMA 缓冲区（物理连续内存）
 *   2. 摄像头 ISP 通过 DMA 直接将像素数据写入这些缓冲区（不经过 CPU）
 *   3. 用户空间通过 mmap() 将内核缓冲区映射到进程地址空间
 *   4. 读取时直接访问 mmap 映射的地址，无需 CPU 拷贝
 *   5. 处理完后 QBUF 归还缓冲区，驱动循环使用
 *
 *   传统方式：内核 buffer → copy_to_user → 用户 buffer（一次 CPU 拷贝）
 *   mmap 方式：内核 buffer ←→ 用户地址空间（同一块物理内存，零拷贝）
 *
 * **epoll 异步等待**
 *   使用 epoll 监听 V4L2 设备 fd，当有新帧到达时 epoll_wait 返回
 *   超时 100ms 确保线程可以定期检查 running/shutdown 标志
 *   避免纯轮询的 CPU 浪费，也避免阻塞式 read 导致无法退出
 *
 * **IMX415 摄像头寄存器优化**
 *   - 曝光值设为 ~90%（2000/2242），保证图像亮度
 *   - 增益设为 0，最小化噪声（IMX415 低光性能优秀）
 */

#include "v4l2.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>          /* mmap / munmap：内存映射 */
#include <sys/epoll.h>         /* epoll：I/O 多路复用 */
#include <linux/videodev2.h>   /* V4L2 核心头文件 */

#define MAX_BUFS 8             /* V4L2 DMA buffer 最大数量 */

/*
 * V4L2 采集器内部状态
 * 封装了设备 fd、mmap 映射的缓冲区、epoll fd 等
 * 对外不透明（opaque pointer pattern），调用者通过 v4l2_cap_t* 操作
 */
struct v4l2_cap_s {
    int fd;                    /* V4L2 设备文件描述符（如 /dev/video0）      */
    int epfd;                  /* epoll 实例文件描述符                       */

    struct {                   /* mmap 映射的 DMA 缓冲区数组                 */
        void*  start;          /* 映射到用户空间的起始虚拟地址               */
        size_t length;         /* 缓冲区字节长度                            */
    } bufs[MAX_BUFS];
    uint32_t nbufs;            /* 实际分配的缓冲区数量                       */

    uint32_t width, height;    /* 采集分辨率                                */
    uint32_t stride_y;         /* Y 平面行步长（bytesperline，可能 > width） */
    uint32_t pixfmt;           /* V4L2 四字符像素格式码                      */
    uint32_t buf_type;         /* V4L2 buffer 类型（SINGLE_PLANE 或 MPLANE） */
    uint32_t nplanes;          /* 多平面模式下的平面数量                     */

    int      mplane;           /* 是否使用多平面 API（V4L2_CAP_VIDEO_CAPTURE_MPLANE） */
    int      running;          /* 采集是否正在运行（流是否已开启）           */
    uint64_t seq;              /* 帧序号计数器（每采集一帧自增）             */
};

/*
 * 带自动重试的 ioctl 包装函数
 * 当 ioctl 被信号中断时（errno == EINTR），自动重试
 * 这在高负载或调试场景下很重要（如 gdb attach 时 SIGSTOP）
 */
static inline int xioctl(int fd, int req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/*
 * 打开并配置 V4L2 设备
 *
 * 完整配置流程：
 *   1. 打开设备节点（O_RDWR | O_NONBLOCK）
 *   2. 查询设备能力（VIDIOC_QUERYCAP）
 *   3. 设置采集格式（VIDIOC_S_FMT）：分辨率、像素格式
 *   4. 设置帧率（VIDIOC_S_PARM）
 *   5. 请求 DMA 缓冲区（VIDIOC_REQBUFS）：MEMORY_MMAP
 *   6. 查询每个缓冲区信息（VIDIOC_QUERYBUF）
 *   7. mmap 映射每个缓冲区到用户空间
 *   8. 设置 IMX415 摄像头控制参数（曝光、增益）
 *
 * @dev：      设备节点路径，如 "/dev/video0"
 * @w, h：     期望的采集分辨率
 * @fps：      期望的帧率
 * @pixfmt：   像素格式字符串 "NV12" / "MJPEG" / "YUYV"
 * @buf_count：DMA 缓冲区数量
 * 返回：v4l2_cap_t* 或 NULL（失败）
 */
v4l2_cap_t* v4l2_open(const char* dev, uint32_t w, uint32_t h,
                       uint32_t fps, const char* pixfmt, uint32_t buf_count) {
    v4l2_cap_t* cap = calloc(1, sizeof(v4l2_cap_t));
    if (!cap) return NULL;
    cap->fd = -1; cap->epfd = -1;  /* 初始化 fd 为无效值，方便 fail 清理 */

    /*
     * 步骤 1：解析像素格式
     * V4L2 使用 v4l2_fourcc 宏生成四字符码（FourCC）
     * NV12 = 'NV12', MJPEG = 'MJPG', YUYV = 'YUYV'
     */
    if (strcasecmp(pixfmt, "NV12") == 0)
        cap->pixfmt = v4l2_fourcc('N','V','1','2');
    else if (strcasecmp(pixfmt, "MJPEG") == 0)
        cap->pixfmt = v4l2_fourcc('M','J','P','G');
    else if (strcasecmp(pixfmt, "YUYV") == 0)
        cap->pixfmt = v4l2_fourcc('Y','U','Y','V');
    else { LOG_ERROR("unknown pixfmt"); goto fail; }

    /*
     * 步骤 2：打开设备节点
     * O_RDWR：读写模式（需要 mmap 写入权限）
     * O_NONBLOCK：非阻塞模式（配合 epoll 使用，避免 open 时阻塞）
     */
    cap->fd = open(dev, O_RDWR | O_NONBLOCK);
    if (cap->fd < 0) { LOG_ERROR("open %s: %s", dev, strerror(errno)); goto fail; }

    /*
     * 步骤 3：查询设备能力
     * V4L2_CAP_DEVICE_CAPS：检查是否支持此查询（新 V4L2 API）
     * V4L2_CAP_VIDEO_CAPTURE_MPLANE：是否支持多平面采集（IMX415 支持）
     * V4L2_CAP_STREAMING：是否支持流式 I/O（mmap 方式的前提）
     */
    struct v4l2_capability vcap;
    memset(&vcap, 0, sizeof(vcap));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &vcap) < 0)
        { LOG_ERROR("QUERYCAP fail"); goto fail; }

    uint32_t caps = (vcap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? vcap.device_caps : vcap.capabilities;
    cap->mplane = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    if (!(caps & V4L2_CAP_STREAMING)) { LOG_ERROR("no streaming"); goto fail; }

    /* 根据是否支持 MPLANE 选择对应的 buffer 类型 */
    cap->buf_type = cap->mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /*
     * 步骤 4：设置采集格式
     * 告诉驱动我们期望的分辨率、像素格式
     * 驱动可能返回与请求不同的值（硬件限制），以返回值为准
     * field = V4L2_FIELD_ANY：由驱动自动选择场模式（逐行/隔行）
     */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = cap->buf_type;
    fmt.fmt.pix_mp.width       = w;
    fmt.fmt.pix_mp.height      = h;
    fmt.fmt.pix_mp.pixelformat = cap->pixfmt;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0)
        { LOG_ERROR("S_FMT fail"); goto fail; }

    /* 以驱动返回的实际值为准（可能与请求不同） */
    cap->width    = fmt.fmt.pix_mp.width;
    cap->height   = fmt.fmt.pix_mp.height;
    cap->stride_y = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;  /* Y平面行步长 */
    cap->nplanes  = fmt.fmt.pix_mp.num_planes;                  /* 平面数（NV12=2） */
    if (cap->nplanes == 0) cap->nplanes = 1;

    /*
     * 步骤 5：设置帧率
     * V4L2_CAP_TIMEPERFRAME：检查驱动是否支持帧率控制
     * timeperframe.numerator / denominator = 每帧时间间隔（秒）
     * 如 1/30 = 每帧约 33.33ms
     */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = cap->buf_type;
    if (xioctl(cap->fd, VIDIOC_G_PARM, &parm) >= 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        xioctl(cap->fd, VIDIOC_S_PARM, &parm);
    }

    /*
     * 步骤 6：请求 DMA 缓冲区
     * V4L2_MEMORY_MMAP：使用 mmap 方式实现零拷贝传输
     * buf_count：请求的缓冲区数量（4~8 个），驱动可能返回不同数量
     *
     * 为什么需要多个缓冲区？
     *   - 摄像头持续输出帧数据，驱动需要循环使用缓冲区
     *   - 用户空间处理一帧时，驱动可以填充下一个缓冲区
     *   - 缓冲区太少 → 容易丢帧；太多 → 浪费内存
     *   - 建议 4~8 个，本配置默认 6 个
     */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = buf_count < MAX_BUFS ? buf_count : MAX_BUFS;
    req.type   = cap->buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0)
        { LOG_ERROR("REQBUFS fail"); goto fail; }
    cap->nbufs = req.count;    /* 驱动实际分配的缓冲区数量 */

    /*
     * 步骤 7：mmap 映射每个缓冲区
     * 对每个缓冲区：
     *   1. VIDIOC_QUERYBUF 获取缓冲区的物理偏移和长度
     *   2. mmap() 将内核 DMA 缓冲区映射到用户空间虚拟地址
     *
     * MAP_SHARED：对映射区域的修改会写回内核缓冲区（共享映射）
     * PROT_READ | PROT_WRITE：可读可写
     */
    for (uint32_t i = 0; i < cap->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type   = cap->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (cap->mplane) { buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES; }
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0)
            { LOG_ERROR("QUERYBUF[%u] fail", i); goto fail; }

        if (cap->mplane) {
            /* 多平面模式：映射第一平面（Y 平面），UV 平面紧随其后 */
            cap->bufs[i].start  = mmap(NULL, planes[0].length, PROT_READ|PROT_WRITE,
                                        MAP_SHARED, cap->fd, planes[0].m.mem_offset);
            cap->bufs[i].length = planes[0].length;
        } else {
            /* 单平面模式：整个缓冲区在一个连续空间中 */
            cap->bufs[i].start  = mmap(NULL, buf.length, PROT_READ|PROT_WRITE,
                                        MAP_SHARED, cap->fd, buf.m.offset);
            cap->bufs[i].length = buf.length;
        }
        if (cap->bufs[i].start == MAP_FAILED) {
            LOG_ERROR("mmap[%u] fail: %s", i, strerror(errno));
            goto fail;
        }
    }

    LOG_INFO("v4l2 opened: %s %ux%u bufs=%u", dev, cap->width, cap->height, cap->nbufs);

    /*
     * 步骤 8：设置 IMX415 摄像头寄存器
     * - V4L2_CID_EXPOSURE_ABSOLUTE：绝对曝光时间（0~2242）
     *   设为 2000 ≈ 90% 最大曝光 → 保证图像亮度
     * - V4L2_CID_GAIN：模拟增益
     *   设为 0 → 最小增益 → 最小噪声（IMX415 低光性能好，不需要高增益）
     *
     * 注意：这些值仅对 IMX415 有效，不同摄像头需要不同的调优值
     */
    {
        struct v4l2_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        ctrl.value = 2000;
        ioctl(cap->fd, VIDIOC_S_CTRL, &ctrl);
        ctrl.id = V4L2_CID_GAIN;
        ctrl.value = 0;
        ioctl(cap->fd, VIDIOC_S_CTRL, &ctrl);
    }

    return cap;

fail:
    v4l2_close(cap);           /* 清理已分配的资源 */
    return NULL;
}

/*
 * 启动视频流采集
 *
 * 流程：
 *   1. QBUF：将所有缓冲区入队到驱动的"待填充"队列
 *   2. STREAMON：启动摄像头硬件流
 *   3. epoll_create1 + epoll_ctl：注册 epoll 监听设备 fd
 *
 * 启动后，摄像头开始向 DMA 缓冲区填充数据
 * 每次有帧就绪时，epoll 会通知我们的采集线程
 */
int v4l2_start(v4l2_cap_t* cap) {
    if (!cap || cap->fd < 0) return -1;

    /*
     * 将所有缓冲区入队（QBUF = Queue Buffer）
     * 驱动现在可以从队列中取出缓冲区来接收摄像头数据
     */
    for (uint32_t i = 0; i < cap->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type   = cap->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (cap->mplane) { buf.m.planes = planes; buf.length = cap->nplanes; }
        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0)
            { LOG_ERROR("QBUF[%u] fail", i); return -1; }
    }

    /* 启动视频流（STREAMON）— 硬件开始采集 */
    int type = cap->buf_type;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0)
        { LOG_ERROR("STREAMON fail"); return -1; }

    /*
     * 创建 epoll 实例并注册设备 fd
     * EPOLLIN：当设备有数据可读（有新帧就绪）时通知
     * epoll_create1(0)：创建 epoll 实例（参数 0 表示不使用 close-on-exec 标志）
     *
     * 为什么用 epoll 而不是 select/poll？
     *   - epoll 是 O(1) 复杂度（select/poll 是 O(n)）
     *   - epoll 支持边缘触发（ET）和水平触发（LT）
     *   - 更适合高并发场景，虽然这里只有一个 fd
     */
    cap->epfd = epoll_create1(0);
    if (cap->epfd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = cap->fd;
        epoll_ctl(cap->epfd, EPOLL_CTL_ADD, cap->fd, &ev);
    }
    cap->running = 1;
    LOG_INFO("v4l2 streaming started (epoll)");
    return 0;
}

/*
 * 停止视频流
 * STREAMOFF → 关闭 epoll → 标记 running=0
 */
void v4l2_stop(v4l2_cap_t* cap) {
    if (!cap || !cap->running) return;
    cap->running = 0;
    int type = cap->buf_type;
    xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    if (cap->epfd >= 0) { close(cap->epfd); cap->epfd = -1; }
}

/*
 * 关闭设备并释放所有资源
 * 顺序：停止流 → munmap 所有缓冲区 → close fd → free 结构体
 */
void v4l2_close(v4l2_cap_t* cap) {
    if (!cap) return;
    v4l2_stop(cap);
    /* munmap：解除 mmap 映射，归还内核 DMA 缓冲区 */
    for (uint32_t i = 0; i < cap->nbufs; i++) {
        if (cap->bufs[i].start && cap->bufs[i].start != MAP_FAILED)
            munmap(cap->bufs[i].start, cap->bufs[i].length);
    }
    if (cap->fd >= 0) close(cap->fd);
    free(cap);
}

/*
 * 采集一帧视频数据
 *
 * 这是采集线程的主循环调用的核心函数
 * 流程：
 *   1. epoll_wait 等待新帧就绪（超时 100ms，用于周期性检查 running 标志）
 *   2. VIDIOC_DQBUF：从驱动的"已完成"队列取出一个填充好的缓冲区
 *   3. 分配 frame_t，从 mmap 区域 memcpy NV12 数据到 frame_t
 *   4. VIDIOC_QBUF：将缓冲区归还驱动的"待填充"队列，循环使用
 *
 * 为什么需要 memcpy 而不是直接传递 mmap 指针？
 *   - mmap 缓冲区是循环使用的，归还后驱动会立即覆盖
 *   - 下游线程（推理/编码）的处理时间不确定，不能依赖 mmap 区域
 *   - memcpy 将数据"固化"到独立内存中，解耦采集和下游处理
 *   - 这是必要的拷贝，后续线程间的传递才是零拷贝（通过 refcount 共享 frame_t）
 *
 * 返回：frame_t*（调用者负责通过 frame_free 释放），或 NULL（超时/停止/错误）
 */
frame_t* v4l2_capture(v4l2_cap_t* cap) {
    if (!cap || !cap->running) return NULL;

    /*
     * epoll_wait 等待设备可读（有新帧就绪）
     * 超时 100ms：确保线程能定期检查 running 和 sig_shutdown 标志
     * 在 30fps（~33ms/帧）下，100ms 超时不会造成明显延迟
     */
    if (cap->epfd >= 0) {
        struct epoll_event ev;
        if (epoll_wait(cap->epfd, &ev, 1, 100) <= 0) return NULL;
    }

    /* 构造 DQBUF 请求 */
    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type   = cap->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (cap->mplane) { buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES; }

    /*
     * VIDIOC_DQBUF（Dequeue Buffer）：从驱动取出一个已填充的缓冲区
     * 成功返回 0；失败返回 -1，errno=EAGAIN 表示暂无数据（非阻塞模式下正常）
     */
    if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) LOG_ERROR("DQBUF fail");
        return NULL;
    }

    /*
     * 计算 NV12 数据的总大小
     * NV12 格式：Y 平面 = stride_y × height 字节，UV 平面 = Y 平面的一半
     * stride_y 可能大于 width（V4L2 驱动对齐要求，如 64 字节对齐）
     * 实际可用像素仍是 width×height，多余部分为填充字节
     */
    size_t ysize = cap->stride_y * cap->height;
    size_t total = ysize + ysize / 2;  /* Y + UV 交错平面 */

    /*
     * 分配 frame_t 并拷贝 NV12 数据
     * calloc 保证所有字段初始化为 0（包括 refcount）
     */
    frame_t* f = calloc(1, sizeof(frame_t));
    if (!f) { xioctl(cap->fd, VIDIOC_QBUF, &buf); return NULL; }

    f->refcount = 1;            /* 初始引用计数 = 1（采集线程持有） */
    f->data = malloc(total);    /* 分配 NV12 数据缓冲区 */
    f->size = total;
    f->width  = cap->width;
    f->height = cap->height;
    f->stride = cap->stride_y;
    f->seq    = cap->seq++;     /* 帧序号自增 */
    f->cap_ts = timestamp_now(); /* 记录采集完成时间戳 */

    if (!f->data) {
        free(f); xioctl(cap->fd, VIDIOC_QBUF, &buf);
        return NULL;
    }

    /*
     * 从 mmap 映射区域拷贝 NV12 数据到 frame_t
     * cap->bufs[buf.index].start：mmap 映射的虚拟地址（内核 DMA 缓冲区）
     * 拷贝完成后立即 QBUF 归还，驱动可以开始填充下一帧
     */
    memcpy(f->data, (uint8_t*)cap->bufs[buf.index].start, total);

    /*
     * QBUF（Queue Buffer）：将缓冲区归还驱动
     * 归还后驱动可以重新使用该缓冲区接收下一帧数据
     */
    xioctl(cap->fd, VIDIOC_QBUF, &buf);
    return f;
}

/* 获取 V4L2 设备 fd（供外部 epoll 等使用） */
int  v4l2_get_fd(v4l2_cap_t* cap) { return cap ? cap->fd : -1; }

/* 检查采集是否正在运行 */
bool v4l2_is_running(v4l2_cap_t* cap) { return cap && cap->running; }
