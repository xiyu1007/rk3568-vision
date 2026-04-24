// main.c
// Created on 2026-04-21

#include "v4l2_capture.h"
#include "video_writer.h"

#include <stdio.h>
#include <unistd.h>  // 提供 sleep/usleep 函数
#include <opencv2/opencv.hpp>

/* 默认配置*/
#define V4L2_DEVICE             "/dev/video0"
#define V4L2_WIDTH              640U
#define V4L2_HEIGHT             480U
#define V4L2_FPS                20U

// /usr/include/linux/videodev2.h
// #define V4L2_PIX_FMT_MJPEG    v4l2_fourcc('M', 'J', 'P', 'G') /* Motion-JPEG   */
#define V4L2_FORMAT             V4L2_PIX_FMT_MJPEG // V4L2_PIX_FMT_NV12
// #define V4L2_FORMAT             V4L2_PIX_FMT_NV12 
#define V4L2_BUFFER_COUNT       4U
#define V4L2_BUFFER_TYPE        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE


int video_show(const v4l2_ctx_t *ctx, const v4l2_buffer_t *f)
{
    cv::Mat img;

    if (ctx->pixfmt == V4L2_PIX_FMT_MJPEG)
    {
        cv::Mat buf(1, f->bytesused[0], CV_8UC1, f->start[0]);
        img = cv::imdecode(buf, cv::IMREAD_COLOR);
    }
    else if (ctx->pixfmt == V4L2_PIX_FMT_NV12)
    {
        cv::Mat yuv(ctx->height * 3 / 2,ctx->width,CV_8UC1,f->start[0]);
        cv::cvtColor(yuv, img, cv::COLOR_YUV2BGR_NV12);
    }

    if (!img.empty())
        cv::imshow("video", img);

    cv::waitKey(1);

    return 0;
}


int frame_cb(v4l2_ctx_t *ctx, const v4l2_buffer_t *f, void *user)
{
    static int id = 0;
    video_ctx_t *v = (video_ctx_t *)user;

    video_write(v, ctx, f);
    if (id < 20)
        save_frame(ctx, f, id);

    video_show(ctx, f);
    id++;
    return 0;
}

int main(int argc, char *argv[]) {

    v4l2_ctx_t ctx = {
        .fd = -1,
        .dev   = (argc > 1) ? argv[1] : V4L2_DEVICE,
        .width = V4L2_WIDTH,
        .height = V4L2_HEIGHT,
        .fps = V4L2_FPS,
        .pixfmt = V4L2_FORMAT,
        .buf_type = V4L2_BUFFER_TYPE,
        .buffers = NULL,
        .n_buffers = V4L2_BUFFER_COUNT,
    };

    if (v4l2_init(&ctx) < 0)
        return -1;
    if (v4l2_start(&ctx) < 0)
        return -1;

    video_ctx_t v;
    video_init(&v, &ctx);

    for (int i = 0; i < 5000; i++)   // 测试100帧
    {
        int ret = v4l2_read(&ctx, frame_cb, &v);
        if (ret < 0){
            V4L2_LOGI("READ err...");
            break;
        }else if (ret == 1) {
            /* EAGAIN，无数据，避免空转 */
            // V4L2_LOGI("busy...");
            usleep(5000);
        }else{
            V4L2_LOGI("cap...");
        }
    }

    v4l2_stop(&ctx);
    return 0;
}