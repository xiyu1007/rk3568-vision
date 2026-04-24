#include "video_writer.h"
#include <stdio.h>
#include <string.h>

int save_frame(const v4l2_ctx_t *ctx,const v4l2_buffer_t *f,int id)
{
    char name[64];
    FILE *fp = NULL;

    switch (ctx->pixfmt)
    {
        case V4L2_PIX_FMT_MJPEG:
            snprintf(name, sizeof(name), "./tmp/frame_%06d.jpg", id);

            fp = fopen(name, "wb");
            if (!fp) return -1;

            fwrite(f->start[0], 1, f->bytesused[0], fp);
            break;

        case V4L2_PIX_FMT_NV12:
            snprintf(name, sizeof(name), "./tmp/frame_%06d.yuv", id);

            fp = fopen(name, "wb");
            if (!fp) return -1;

            if (f->n_planes == 1) {
                fwrite(f->start[0], 1, f->bytesused[0], fp);
            } else if (f->n_planes == 2) {
                fwrite(f->start[0], 1, ctx->width * ctx->height, fp);
                fwrite(f->start[1], 1, ctx->width * ctx->height / 2, fp);
            }
            break;
        default:
            return -1;
    }
    fclose(fp);
    return 0;
}


int video_init(video_ctx_t *v, v4l2_ctx_t *ctx)
{
    if (v->init)
        return 0;
        
    char cmd[256];
    switch (ctx->pixfmt)
    {
        case V4L2_PIX_FMT_NV12:
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y "                 // 覆盖已有输出文件（无需确认）
                "-f rawvideo "              // 输入是裸视频流（无封装/无头）
                "-pix_fmt nv12 "            // 输入像素格式：NV12
                "-s %dx%d "                 // 分辨率：width x height
                "-r %d "                    // 输入帧率（时间基准）
                "-i - "                     // 从 stdin 读取数据（管道输入）
                "-c:v libx264 "             // 使用 H.264 编码器
                "out.mp4",                  // 输出文件（MP4 封装）
                ctx->width, ctx->height, ctx->fps);
            break;

        case V4L2_PIX_FMT_MJPEG:
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y "                 // 覆盖已有输出文件
                "-f mjpeg "                 // 输入为 MJPEG（每帧是完整 JPEG）
                "-r %d "                    // 输入帧率
                "-i - "                     // 从 stdin 读取
                "-c:v libx264 "             // 转码为 H.264
                "out.mp4",                  // 输出 MP4 文件
                ctx->fps);
            break;
        default:
            return -1;
    }

    v->pipe = popen(cmd, "w"); // 接收的是“字符串命令”， 执行命令 + 建立管道 ✔
    v->init = (v->pipe != NULL);

    return v->init ? 0 : -1;
}

/* 写一帧到 ffmpeg stdin */
int video_write(video_ctx_t *v,const v4l2_ctx_t *ctx,const v4l2_buffer_t *f)
{
    if (!v->init) return -1;  // pipe 未初始化，不能写
    switch (ctx->pixfmt)
    {
        case V4L2_PIX_FMT_MJPEG:
            fwrite(f->start[0], 1, f->bytesused[0], v->pipe);
            break;
        case V4L2_PIX_FMT_NV12:
            if (f->n_planes == 1) 
                fwrite(f->start[0], 1, f->bytesused[0], v->pipe);
            else if (f->n_planes == 2) { // NV12
                fwrite(f->start[0], 1, ctx->width * ctx->height, v->pipe);       // 写 Y
                fwrite(f->start[1], 1, ctx->width * ctx->height / 2, v->pipe);   // 写 UV
            }
            break;
        default:
            return -1;  // 不支持的格式
    }

    return 0;
}

void video_close(video_ctx_t *v)
{
    if (v->pipe) {
        pclose(v->pipe);
        v->pipe = NULL;
    }
    v->init = 0;
}