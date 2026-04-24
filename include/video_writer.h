#ifndef __VIDEO_WRITER_H__
#define __VIDEO_WRITER_H__

#include "v4l2_capture.h"
#include <stdio.h>  // 添加这一行

EXTERN_C_BEGIN

typedef struct {
    FILE *pipe;
    int   init;
} video_ctx_t;

int video_init(video_ctx_t *v, v4l2_ctx_t *ctx);

int save_frame(const v4l2_ctx_t *ctx,const v4l2_buffer_t *f,int id);

int video_write(video_ctx_t *v, const v4l2_ctx_t *ctx, const v4l2_buffer_t *f);

void video_close(video_ctx_t *v);

EXTERN_C_END

#endif /* __VIDEO_WRITER_H__ */