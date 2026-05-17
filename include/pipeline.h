#ifndef PIPELINE_H
#define PIPELINE_H

#include "types.h"
#include "ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIPELINE_QUEUE_CAP 8
#define PIPELINE_FRAME_POOL 16

typedef struct pipeline_s pipeline_t;

pipeline_t* pipeline_create(const app_cfg_t* cfg);
int         pipeline_start(pipeline_t* p);
void        pipeline_stop(pipeline_t* p);
int         pipeline_running(pipeline_t* p);

#ifdef __cplusplus
}
#endif
#endif
