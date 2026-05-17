#ifndef RKNN_H
#define RKNN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef X86_DEBUG
typedef struct { int32_t scale, zp; int32_t dims[4], fmt, type, n_dims; char name[64]; } rknn_tensor_attr;
typedef struct { void* buf; uint32_t size; uint8_t want_float, is_prealloc; int32_t index; } rknn_output;
typedef struct { void* buf; uint32_t type, size, fmt; uint8_t pass_through; int32_t index; } rknn_input;
typedef void* rknn_context;
#define RKNN_TENSOR_UINT8 2
#define RKNN_TENSOR_NHWC  1
#define RKNN_TENSOR_NCHW  0
#else
#include "rknn_api.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rknn_ctx_s rknn_ctx_t;

rknn_ctx_t* rknn_ctx_open(const char* model_path, uint32_t npu_core);
void        rknn_ctx_close(rknn_ctx_t* ctx);
int         rknn_ctx_set_inputs(rknn_ctx_t* ctx, const rknn_input* inputs, uint32_t n);
int         rknn_ctx_run(rknn_ctx_t* ctx);
int         rknn_ctx_get_outputs(rknn_ctx_t* ctx, rknn_output* outputs, uint32_t n);
int         rknn_ctx_release_outputs(rknn_ctx_t* ctx, rknn_output* outputs, uint32_t n);
uint32_t    rknn_ctx_input_w(rknn_ctx_t* ctx);
uint32_t    rknn_ctx_input_h(rknn_ctx_t* ctx);
uint32_t    rknn_ctx_input_c(rknn_ctx_t* ctx);
uint32_t    rknn_ctx_output_count(rknn_ctx_t* ctx);
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* ctx, uint32_t idx);

#ifdef __cplusplus
}
#endif
#endif
