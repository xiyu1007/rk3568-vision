#ifndef __VIDEO_WRITER_H__
#define __VIDEO_WRITER_H__


#pragma once

#include "v4l2_capture.h"

typedef struct {
    FILE *pipe;
    
    int   init;
} video_ctx_t;

int video_init(video_ctx_t *v, v4l2_ctx_t *ctx);

int video_write(video_ctx_t *v, const v4l2_ctx_t *ctx, const v4l2_buffer_t *f);

int video_show(const v4l2_ctx_t *ctx, const v4l2_buffer_t *f);

void video_close(video_ctx_t *v);


#endif /* __VIDEO_WRITER_H__ */