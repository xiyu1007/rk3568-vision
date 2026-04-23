#ifndef __V4L2_LOG_H__
#define __V4L2_LOG_H__

/* ================= 配置 ================= */
#include "stdio.h"
#include <time.h>       

// 优先级：DEBUG > LOG > 关闭
#if defined(V4L2_DEBUG_ENABLE)
    // 调试模式：输出到控制台
    #define V4L2_LOGI(fmt, ...) printf("[V4L2][I] " fmt "\n", ##__VA_ARGS__)
    #define V4L2_LOGE(fmt, ...) printf("[V4L2][E] " fmt "\n", ##__VA_ARGS__)

#elif defined(V4L2_LOG_ENABLE)
    // 日志模式：写入文件
    #include "log.h"
    // log.h 中声明了 logi 和 loge
    #define V4L2_LOGI logi
    #define V4L2_LOGE loge

#else
    // 关闭所有日志：空宏
    #define V4L2_LOGI(fmt, ...) ((void)0 )
    #define V4L2_LOGE(fmt, ...) ((void)0 )
#endif


#endif /* __V4L2_LOG_H__ */


