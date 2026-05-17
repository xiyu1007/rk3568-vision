#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*enc_packet_cb)(void* userdata, const uint8_t* data, size_t size,
                               int64_t pts, int keyframe);

typedef struct encoder_s encoder_t;

encoder_t* encoder_open(uint32_t width, uint32_t height, uint32_t fps,
                        uint32_t bitrate, uint32_t gop_size);
void       encoder_close(encoder_t* e);
int        encoder_encode(encoder_t* e, const uint8_t* nv12, int64_t pts);
int        encoder_flush(encoder_t* e);
void       encoder_set_callback(encoder_t* e, enc_packet_cb cb, void* userdata);
int        encoder_ready(encoder_t* e);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
