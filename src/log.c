#include "log.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sys/time.h>
#include <pthread.h>


static FILE *g_fp = NULL;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_init_done = 0;   // 关键：只初始化一次

/* 时间戳(ms) */
/* 时间戳(year:month:day:min:sec) */
static inline void get_timestamp(char *buffer, size_t size)
{
    struct timeval tv;
    struct tm *tm_info;
    
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    
    strftime(buffer, size, "%Y:%m:%d|%M:%S", tm_info);
}

/* 自动初始化（首次调用触发 inline） */
static inline void log_init(void)
{
    mkdir("./tmp", 0755);
    if (!g_fp)
        g_fp = fopen(LOG_FILE, "a+");
}

/* 核心日志 */
static void log_write(const char *lv, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&g_mtx);

    if (!g_fp && !g_init_done)
        log_init();

    if (g_fp)
    {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        #ifndef LOG_APP
        fprintf(g_fp, "[%s] [%s] ", timestamp, lv);
        #else
        fprintf(g_fp, "[%s] [%s] [%s] ", timestamp, lv , LOG_APP);
        #endif
        vfprintf(g_fp, fmt, ap);
        fprintf(g_fp, "\n");
        fflush(g_fp);
    }

    pthread_mutex_unlock(&g_mtx);
}

/* INFO */
void logi(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); // 初始化，指向第一个可变参数
    log_write("I", fmt, ap);
    va_end(ap);
}

/* ERROR */
void loge(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("E", fmt, ap);
    va_end(ap);
}
