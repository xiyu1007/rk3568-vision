#include "video_writer.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
// #include <sys/types.h>
#include <sys/wait.h>
// #include <fcntl.h>
// #include <stdio.h>
// #include <string.h>
// #include <errno.h>
#include <opencv2/opencv.hpp>

int video_init(video_ctx_t *v, const v4l2_ctx_t *ctx)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;   // pipefd[0]=读端, pipefd[1]=写端

    pid_t pid = vfork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        // ---- child ----
        // int dup2(int oldfd, int newfd);
        // 让 newfd 指向 oldfd 所指向的同一个底层文件对象（共享文件偏移量、状态标志等）。
        // stdin ← pipe read
        if (dup2(pipefd[0], STDIN_FILENO) < 0)
            _exit(127);

        // 关闭多余 fd
        close(pipefd[0]);
        close(pipefd[1]);

        // 根据 pixfmt 构造 argv
        switch (ctx->pixfmt)
        {
        case V4L2_PIX_FMT_NV12: {
            char size[32];
            char fps[16];

            snprintf(size, sizeof(size), "%dx%d", ctx->width, ctx->height);
            snprintf(fps,  sizeof(fps),  "%d",   ctx->fps);

            const char *argv[] = { // 等价 const char * * argv
                "ffmpeg", "-y",
                "-f", "rawvideo",
                "-pix_fmt", "nv12",
                "-s", size,
                "-r", fps,
                "-i", "-",
                "-c:v", "libx264",
                "-", // 输出到标准输出
                // "out.mp4",
                NULL
            };

            execvp("ffmpeg", (char * const *)argv); // 等价  char * const * argv
            _exit(127);
        }
        case V4L2_PIX_FMT_MJPEG: {
            char fps[16];

            snprintf(fps, sizeof(fps), "%d", ctx->fps);

            const char *argv[] = {
                "ffmpeg", "-y",
                "-f", "mjpeg",
                "-r", fps,
                "-i", "-",
                "-c:v", "libx264",
                "out.mp4", // 输出到标准输出 out.mp4
                NULL
            };

            execvp("ffmpeg", (char * const *)argv);
            _exit(127);
        }
        default:
            _exit(127);
        }

        _exit(127); // exec 失败
    }

    // ---- parent ----
    close(pipefd[0]);          // 关闭读端
    v->fd   = pipefd[1];       // 保留写端
    v->pid  = pid;
    v->init = 1;
    return 0;
}

int video_write(video_ctx_t *v,const v4l2_ctx_t *ctx,const v4l2_buffer_t *f)
{
    if (!v->init) return -1;

    switch (ctx->pixfmt)
    {
        case V4L2_PIX_FMT_MJPEG:
            write(v->fd, f->start[0], f->bytesused[0]);
            break;
        case V4L2_PIX_FMT_NV12:
            if (f->n_planes == 1) {
                write(v->fd, f->start[0], f->bytesused[0]);
            } else {
                write(v->fd, f->start[0], f->bytesused[0]);
                write(v->fd, f->start[1], f->bytesused[1]);
                // write(v->fd, f->start[0], ctx->width * ctx->height);
                // write(v->fd, f->start[1], ctx->width * ctx->height / 2);
            }
            break;
        default:
            return -1;  // 不支持的格式
    }
    return 0;
}

void video_close(video_ctx_t *v)
{
    if (!v->init) return;

    close(v->fd);          // 关闭 stdin → ffmpeg 收到 EOF
    waitpid(v->pid, NULL, 0); // 等待退出（写 MP4 尾部）
    v->init = 0;
}