#ifndef __COMMON_H__
#define __COMMON_H__


#ifdef __cplusplus
    #define EXTERN_C_BEGIN extern "C" {
    #define EXTERN_C_END }
#else
    #define EXTERN_C_BEGIN
    #define EXTERN_C_END
#endif

#ifndef LOG_ENABLE
#define LOG_ENABLE
#endif

// 优先级：DEBUG > LOG > 关闭
#if defined(DEBUG_ENABLE)
    // 调试模式：输出到控制台
    // #define V4L2_LOGI(fmt, ...) printf("[V4L2][I] " fmt "\n", ##__VA_ARGS__)
    // #define V4L2_LOGE(fmt, ...) printf("[V4L2][E] " fmt "\n", ##__VA_ARGS__)
    #define LOGI(fmt, ...) do { printf("[LOG] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)
    #define LOGE(fmt, ...) do { printf("[LOG] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)

#elif defined(LOG_ENABLE)
    // 日志模式：写入文件
    #define LOG_FILE "./tmp/main.log"
    #include "log.h"
    // log.h 中声明了 logi 和 loge
    #define LOGI logi
    #define LOGE loge
#else
    // 关闭所有日志：空宏
    #define LOGI(fmt, ...) ((void)0 )
    #define LOGE(fmt, ...) ((void)0 )
#endif

#ifdef __GNUC__
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

#endif /* __COMMON_H__ */