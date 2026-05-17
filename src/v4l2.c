#include "v4l2.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <linux/videodev2.h>

#define MAX_BUFS 8

struct v4l2_cap_s {
    int fd, epfd;
    struct { void* start; size_t length; } bufs[MAX_BUFS];
    uint32_t nbufs, width, height, stride_y;
    uint32_t pixfmt, buf_type;
    int mplane, running;
    uint64_t seq;
};

static int xioctl(int fd, int req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

v4l2_cap_t* v4l2_open(const char* dev, uint32_t w, uint32_t h,
                       uint32_t fps, const char* pixfmt, uint32_t buf_count) {
    v4l2_cap_t* cap = calloc(1, sizeof(v4l2_cap_t));
    if (!cap) return NULL;
    cap->fd = -1; cap->epfd = -1;

    if (strcasecmp(pixfmt, "NV12") == 0)
        cap->pixfmt = v4l2_fourcc('N','V','1','2');
    else if (strcasecmp(pixfmt, "MJPEG") == 0)
        cap->pixfmt = v4l2_fourcc('M','J','P','G');
    else if (strcasecmp(pixfmt, "YUYV") == 0)
        cap->pixfmt = v4l2_fourcc('Y','U','Y','V');
    else { LOG_ERROR("unknown pixfmt"); goto fail; }

    cap->fd = open(dev, O_RDWR | O_NONBLOCK);
    if (cap->fd < 0) { LOG_ERROR("open %s: %s", dev, strerror(errno)); goto fail; }

    struct v4l2_capability vcap;
    memset(&vcap, 0, sizeof(vcap));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &vcap) < 0)
        { LOG_ERROR("QUERYCAP fail"); goto fail; }

    uint32_t caps = (vcap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? vcap.device_caps : vcap.capabilities;
    cap->mplane = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    if (!(caps & V4L2_CAP_STREAMING)) { LOG_ERROR("no streaming"); goto fail; }

    cap->buf_type = cap->mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = cap->buf_type;
    fmt.fmt.pix_mp.width       = w;
    fmt.fmt.pix_mp.height      = h;
    fmt.fmt.pix_mp.pixelformat = cap->pixfmt;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0)
        { LOG_ERROR("S_FMT fail"); goto fail; }

    cap->width  = fmt.fmt.pix_mp.width;
    cap->height = fmt.fmt.pix_mp.height;
    cap->stride_y = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = cap->buf_type;
    if (xioctl(cap->fd, VIDIOC_G_PARM, &parm) >= 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        xioctl(cap->fd, VIDIOC_S_PARM, &parm);
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = buf_count < MAX_BUFS ? buf_count : MAX_BUFS;
    req.type   = cap->buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0)
        { LOG_ERROR("REQBUFS fail"); goto fail; }
    cap->nbufs = req.count;

    for (uint32_t i = 0; i < cap->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cap->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cap->mplane) { buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES; }
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0)
            { LOG_ERROR("QUERYBUF[%u] fail", i); goto fail; }
        off_t  off = cap->mplane ? planes[0].m.mem_offset : buf.m.offset;
        size_t len = cap->mplane ? planes[0].length : buf.length;
        cap->bufs[i].start = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, cap->fd, off);
        cap->bufs[i].length = len;
        if (cap->bufs[i].start == MAP_FAILED)
            { LOG_ERROR("mmap[%u] fail", i); goto fail; }
    }

    LOG_INFO("v4l2 opened: %s %ux%u bufs=%u", dev, cap->width, cap->height, cap->nbufs);
    return cap;

fail:
    v4l2_close(cap);
    return NULL;
}

int v4l2_start(v4l2_cap_t* cap) {
    if (!cap || cap->fd < 0) return -1;
    for (uint32_t i = 0; i < cap->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cap->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cap->mplane) { buf.m.planes = planes; buf.length = 1; }
        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0)
            { LOG_ERROR("QBUF[%u] fail", i); return -1; }
    }
    int type = cap->buf_type;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0)
        { LOG_ERROR("STREAMON fail"); return -1; }

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

void v4l2_stop(v4l2_cap_t* cap) {
    if (!cap || !cap->running) return;
    cap->running = 0;
    int type = cap->buf_type;
    xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    if (cap->epfd >= 0) { close(cap->epfd); cap->epfd = -1; }
}

void v4l2_close(v4l2_cap_t* cap) {
    if (!cap) return;
    v4l2_stop(cap);
    for (uint32_t i = 0; i < cap->nbufs; i++) {
        if (cap->bufs[i].start && cap->bufs[i].start != MAP_FAILED)
            munmap(cap->bufs[i].start, cap->bufs[i].length);
    }
    if (cap->fd >= 0) close(cap->fd);
    free(cap);
}

frame_t* v4l2_capture(v4l2_cap_t* cap) {
    if (!cap || !cap->running) return NULL;

    if (cap->epfd >= 0) {
        struct epoll_event ev;
        if (epoll_wait(cap->epfd, &ev, 1, 100) <= 0) return NULL;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cap->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (cap->mplane) { buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES; }

    if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) LOG_ERROR("DQBUF fail");
        return NULL;
    }

    size_t ysize = cap->stride_y * cap->height;
    size_t total = ysize + ysize / 2;

    frame_t* f = calloc(1, sizeof(frame_t));
    if (!f) { xioctl(cap->fd, VIDIOC_QBUF, &buf); return NULL; }

    f->refcount = 1;
    f->data = malloc(total);
    f->size = total;
    f->width  = cap->width;
    f->height = cap->height;
    f->stride = cap->stride_y;
    f->seq    = cap->seq++;
    f->cap_ts = timestamp_now();

    if (!f->data) {
        free(f); xioctl(cap->fd, VIDIOC_QBUF, &buf);
        return NULL;
    }

    memcpy(f->data, (uint8_t*)cap->bufs[buf.index].start, total);
    xioctl(cap->fd, VIDIOC_QBUF, &buf);
    return f;
}

int  v4l2_get_fd(v4l2_cap_t* cap) { return cap ? cap->fd : -1; }
bool v4l2_is_running(v4l2_cap_t* cap) { return cap && cap->running; }
