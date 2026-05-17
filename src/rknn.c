#ifdef X86_DEBUG

#include "rknn.h"
#include "logger.h"
#include <stdlib.h>

struct rknn_ctx_s { int stub; };

rknn_ctx_t* rknn_ctx_open(const char* path, uint32_t core) {
    (void)path; (void)core;
    LOG_WARN("RKNN not available on x86");
    return NULL;
}
void rknn_ctx_close(rknn_ctx_t* c) { free(c); }
int rknn_ctx_set_inputs(rknn_ctx_t* c, const rknn_input* i, uint32_t n) { (void)c;(void)i;(void)n; return 0; }
int rknn_ctx_run(rknn_ctx_t* c) { (void)c; return 0; }
int rknn_ctx_get_outputs(rknn_ctx_t* c, rknn_output* o, uint32_t n) { (void)c;(void)o;(void)n; return 0; }
int rknn_ctx_release_outputs(rknn_ctx_t* c, rknn_output* o, uint32_t n) { (void)c;(void)o;(void)n; return 0; }
uint32_t rknn_ctx_input_w(rknn_ctx_t* c)  { (void)c; return 640; }
uint32_t rknn_ctx_input_h(rknn_ctx_t* c)  { (void)c; return 640; }
uint32_t rknn_ctx_input_c(rknn_ctx_t* c)  { (void)c; return 3; }
uint32_t rknn_ctx_output_count(rknn_ctx_t* c) { (void)c; return 3; }
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* c, uint32_t i) { (void)c;(void)i; return NULL; }

#else

#include "rknn.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rknn_ctx_s {
    rknn_context       ctx;
    uint8_t*           model_data;
    size_t             model_size;
    uint32_t           in_w, in_h, in_c;
    uint32_t           out_n;
    rknn_input_output_num io_num;
    rknn_tensor_attr*  input_attrs;
    rknn_tensor_attr*  output_attrs;
};

static uint8_t* load_model_file(const char* path, size_t* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG_ERROR("cannot open model: %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(sz);
    if (data && fread(data, 1, sz, f) != (size_t)sz) {
        free(data); fclose(f);
        return NULL;
    }
    fclose(f);
    *out_sz = (size_t)sz;
    LOG_INFO("model loaded: %s (%ld bytes)", path, sz);
    return data;
}

rknn_ctx_t* rknn_ctx_open(const char* model_path, uint32_t npu_core) {
    rknn_ctx_t* c = calloc(1, sizeof(rknn_ctx_t));
    if (!c) return NULL;

    c->model_data = load_model_file(model_path, &c->model_size);
    if (!c->model_data) { free(c); return NULL; }

    int ret = rknn_init(&c->ctx, c->model_data, c->model_size, 0, NULL);
    if (ret < 0) { LOG_ERROR("rknn_init failed: %d", ret); free(c->model_data); free(c); return NULL; }

    rknn_set_core_mask(c->ctx, npu_core == 0 ? RKNN_NPU_CORE_0 :
                                npu_core == 1 ? RKNN_NPU_CORE_1 : RKNN_NPU_CORE_2);

    ret = rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &c->io_num, sizeof(c->io_num));
    if (ret < 0) { LOG_ERROR("rknn_query IO failed"); rknn_ctx_close(c); return NULL; }
    c->out_n = c->io_num.n_output;
    LOG_INFO("RKNN IO: %u in, %u out", c->io_num.n_input, c->io_num.n_output);

    c->input_attrs = calloc(c->io_num.n_input, sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < c->io_num.n_input; i++) {
        c->input_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_INPUT_ATTR, &c->input_attrs[i], sizeof(rknn_tensor_attr));
    }

    c->output_attrs = calloc(c->io_num.n_output, sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < c->io_num.n_output; i++) {
        c->output_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &c->output_attrs[i], sizeof(rknn_tensor_attr));
    }

    rknn_tensor_attr* a = &c->input_attrs[0];
    if (a->fmt == RKNN_TENSOR_NCHW) {
        c->in_c = a->dims[1]; c->in_h = a->dims[2]; c->in_w = a->dims[3];
    } else {
        c->in_h = a->dims[1]; c->in_w = a->dims[2]; c->in_c = a->dims[3];
    }
    LOG_INFO("model input: %ux%ux%u", c->in_w, c->in_h, c->in_c);
    return c;
}

void rknn_ctx_close(rknn_ctx_t* c) {
    if (!c) return;
    if (c->ctx) rknn_destroy(c->ctx);
    free(c->model_data);
    free(c->input_attrs);
    free(c->output_attrs);
    free(c);
}

int rknn_ctx_set_inputs(rknn_ctx_t* c, const rknn_input* inputs, uint32_t n) {
    return c && rknn_inputs_set(c->ctx, n, (rknn_input*)inputs) >= 0;
}
int rknn_ctx_run(rknn_ctx_t* c) {
    return c && rknn_run(c->ctx, NULL) >= 0;
}
int rknn_ctx_get_outputs(rknn_ctx_t* c, rknn_output* outputs, uint32_t n) {
    return c && rknn_outputs_get(c->ctx, n, outputs, NULL) >= 0;
}
int rknn_ctx_release_outputs(rknn_ctx_t* c, rknn_output* outputs, uint32_t n) {
    return c && rknn_outputs_release(c->ctx, n, outputs) >= 0;
}
uint32_t rknn_ctx_input_w(rknn_ctx_t* c)  { return c ? c->in_w  : 640; }
uint32_t rknn_ctx_input_h(rknn_ctx_t* c)  { return c ? c->in_h  : 640; }
uint32_t rknn_ctx_input_c(rknn_ctx_t* c)  { return c ? c->in_c  : 3; }
uint32_t rknn_ctx_output_count(rknn_ctx_t* c) { return c ? c->out_n : 3; }
const rknn_tensor_attr* rknn_ctx_output_attr(rknn_ctx_t* c, uint32_t idx) {
    return (c && c->output_attrs && idx < c->out_n) ? &c->output_attrs[idx] : NULL;
}

#endif
