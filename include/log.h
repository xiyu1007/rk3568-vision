#ifndef __LOG_H__
#define __LOG_H__


/* ================= 配置 ================= */
#ifndef LOG_FILE
#define LOG_FILE "./tmp/log.log"
#endif

#ifndef LOG_APP
#define LOG_APP "V4L2"
#endif

extern void logi(const char *fmt, ...);
extern void loge(const char *fmt, ...);

#endif /* __LOG_H__ */