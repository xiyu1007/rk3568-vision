#ifndef BRIDGE_H
#define BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void* bridge_detector_create(void);
void  bridge_detector_destroy(void* d);
int   bridge_detector_init(void* d, const char* model, const char* labels,
                            float conf, float nms, uint32_t npu_core);
void  bridge_detector_detect(void* d, const uint8_t* bgr, int w, int h,
                              detect_result_t* result);
int   bridge_detector_input_w(void* d);
int   bridge_detector_input_h(void* d);

void* bridge_display_create(const char* name);
void  bridge_display_destroy(void* d);
void  bridge_display_show(void* d, const uint8_t* bgr, int w, int h,
                           int show_fps, double fps,
                           const detect_result_t* detections);

void  bridge_nv12_to_bgr(const uint8_t* nv12, int stride, int w, int h, uint8_t* bgr_out);

#ifdef __cplusplus
}
#endif
#endif
