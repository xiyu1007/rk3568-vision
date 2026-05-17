#ifndef RTMP_H
#define RTMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtmp_s rtmp_t;

rtmp_t* rtmp_open(const char* url, uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate);
void    rtmp_close(rtmp_t* r);
int     rtmp_push_video(rtmp_t* r, const uint8_t* data, size_t size,
                         int64_t pts, int keyframe);
int     rtmp_is_connected(rtmp_t* r);

#ifdef __cplusplus
}
#endif
#endif
