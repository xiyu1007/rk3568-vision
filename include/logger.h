/*
 * ==========================================================================
 * logger.h — 异步日志模块头文件
 * ==========================================================================
 *
 * **日志级别**（从低到高）：
 *   DEBUG(0) → INFO(1) → WARN(2) → ERROR(3) → FATAL(4)
 *
 * **使用方式**：
 *   LOG_INFO("capture: %s %ux%u", dev, w, h);
 *   LOG_ERROR("v4l2_open failed: %s", strerror(errno));
 *
 * **异步写入**：
 *   日志消息先入队（环形缓冲区 4096 条），专用写线程异步写入文件
 *   避免磁盘 I/O 阻塞业务线程，确保实时性
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3
#define LOG_LEVEL_FATAL 4

/* 初始化日志系统 */
void logger_init(const char* file_path, int min_level, int console, int async);

/* 关闭日志系统 */
void logger_shutdown(void);

/* 写入一条日志（底层函数，通常通过宏调用） */
void logger_write(int level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));  /* GCC/Clang printf 格式检查 */

/* 便捷宏：自动填充 __FILE__ 和 __LINE__ */
#define LOG_DEBUG(fmt, ...) logger_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  logger_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  logger_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) logger_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
