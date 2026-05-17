#ifndef PERF_H
#define PERF_H

#include "types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern perf_t g_perf;

void perf_record_capture(int64_t us);
void perf_record_infer(int64_t us);
void perf_record_encode(int64_t us);
void perf_record_drop(void);
void perf_report(void);

#ifdef __cplusplus
}
#endif

#endif /* PERF_H */
