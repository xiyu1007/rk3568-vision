#ifndef V4L2_H
#define V4L2_H

#include "types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v4l2_cap_s v4l2_cap_t;

v4l2_cap_t* v4l2_open(const char* device, uint32_t width, uint32_t height,
                       uint32_t fps, const char* pixfmt, uint32_t buf_count);
int         v4l2_start(v4l2_cap_t* cap);
void        v4l2_stop(v4l2_cap_t* cap);
void        v4l2_close(v4l2_cap_t* cap);

frame_t*    v4l2_capture(v4l2_cap_t* cap);
int         v4l2_get_fd(v4l2_cap_t* cap);
bool        v4l2_is_running(v4l2_cap_t* cap);

#ifdef __cplusplus
}
#endif

#endif /* V4L2_H */
