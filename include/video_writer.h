#ifndef __VIDEO_WRITER_H__
#define __VIDEO_WRITER_H__

#include "v4l2_capture.h"

#include <sys/types.h>


typedef struct {
    int   fd;    // 写端（给 ffmpeg 的 stdin）
    pid_t pid;   // 子进程
    int   init;
} video_ctx_t;

int video_init(video_ctx_t *v, const v4l2_ctx_t *ctx);
int video_write(video_ctx_t *v, const v4l2_ctx_t *ctx, const v4l2_buffer_t *f);

void video_close(video_ctx_t *v);

int video_show(video_ctx_t *v, const v4l2_ctx_t *ctx);

#endif /* __VIDEO_WRITER_H__ */