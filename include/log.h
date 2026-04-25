#ifndef __LOG_H__
#define __LOG_H__

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 配置 ================= */
#ifndef LOG_FILE
#define LOG_FILE "./tmp/log.log"
#endif

// #ifndef LOG_APP
// #define LOG_APP "V4L2"
// #endif

void logi(const char *fmt, ...);
void loge(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __LOG_H__ */