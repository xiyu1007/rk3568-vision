/*
 * ==========================================================================
 * logger.c — 异步线程安全日志模块
 * ==========================================================================
 *
 * **设计目标**：
 *   - 生产-消费模式：调用线程（生产者）快速入队后立即返回
 *   - 专用写线程（消费者）从队列中批量取日志写入文件和终端
 *   - 避免日志 I/O 阻塞主业务流程（尤其是磁盘写入的延迟）
 *
 * **架构**：
 *
 *   ┌──────────┐   push   ┌─────────────┐   pop   ┌──────────┐
 *   │ 调用线程  │ ───────→ │ 环形缓冲区    │ ───────→ │ 写线程    │
 *   │ (业务代码) │          │ (4096 entries)│          │ (writer)  │
 *   └──────────┘          └─────────────┘          └────┬─────┘
 *                                                      │
 *                                               ┌──────┴──────┐
 *                                               │ 日志文件      │
 *                                               │ + stderr     │
 *                                               └─────────────┘
 *
 * **环形缓冲区**：
 *   - 大小 4096 entries × ~512 bytes = ~2MB（足够30fps × 多线程使用）
 *   - 使用 pthread_mutex + pthread_cond 实现生产者-消费者同步
 *   - 写线程批量取日志（最多 64 条/批），减少锁竞争
 *
 * **异步 vs 同步模式**：
 *   - async=true（默认）：日志入队后立即返回，写线程异步写入
 *     ✓ 高性能，不阻塞业务线程
 *     ✗ 程序崩溃时可能丢失队列中的日志
 *   - async=false：同步模式，直接写入文件/终端
 *     ✓ 不会丢失日志
 *     ✗ I/O 阻塞影响性能
 *
 * **日志级别**：
 *   DEBUG(0) → INFO(1) → WARN(2) → ERROR(3) → FATAL(4)
 *   低于 min_level 的日志会被直接丢弃（不入队）
 */

#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>            /* mkdir */
#include <sys/types.h>

#define MSG_MAX     512          /* 单条日志消息最大长度 */
#define QUEUE_SIZE  4096         /* 环形缓冲区容量（条目数） */

/* 日志条目结构体 */
typedef struct {
    int   level;                 /* 日志级别（0-4） */
    char  msg[MSG_MAX];          /* 已格式化的日志消息 */
} log_entry_t;

/* ── 环形缓冲区 ──────────────────────────────────────────────────────── */
static log_entry_t g_queue[QUEUE_SIZE];
static volatile int g_wr = 0;    /* 生产者写指针（调用线程写入位置） */
static volatile int g_rd = 0;    /* 消费者读指针（写线程读取位置） */

/* 生产-消费同步原语 */
static pthread_mutex_t  g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t        g_thread;     /* 写线程句柄 */
static volatile int     g_running = 0; /* 写线程运行标志 */

/* 日志配置 */
static FILE*      g_file    = NULL;   /* 日志文件句柄 */
static int        g_min_lvl = LOG_LEVEL_INFO; /* 最低输出级别 */
static int        g_console = 1;      /* 是否输出到控制台 */
static int        g_async   = 1;      /* 是否异步写 */

/* ── 文件操作辅助 ────────────────────────────────────────────────────── */

/* 确保日志文件所在目录存在（递归创建） */
static void ensure_log_dir(const char* path) {
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);        /* 创建目录（如已存在则忽略） */
    }
}

/* ── 写线程 ──────────────────────────────────────────────────────────── */

/*
 * 日志写线程主循环
 *
 * 职责：
 *   1. 等待环形缓冲区有数据（pthread_cond_timedwait, 100ms 超时）
 *   2. 批量取出日志（最多 64 条/批），减少锁获取次数
 *   3. 写入日志文件 + stderr
 *
 * 批量写入策略：
 *   为什么要批量？
 *     - 减少 pthread_mutex_lock/unlock 次数（每次获取/释放锁 ~100ns）
 *     - 减少 fflush 次数（批量写后在循环结束后统一 flush）
 *   但批量不宜太大 → 64 条是平衡点（约 32KB 数据）
 */
static void* writer_thread(void* arg) {
    (void)arg;
    log_entry_t batch[64];       /* 批量缓冲区 */

    while (g_running) {
        /*
         * 等待队列中有数据
         * pthread_cond_timedwait：超时 100ms
         *   超时后无论是否有数据都会醒来，检查 g_running 标志
         *   避免纯 cond_wait 导致的无法退出问题
         */
        pthread_mutex_lock(&g_mtx);
        while (g_rd == g_wr && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;  /* +100ms */
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
        }

        /* 批量取日志（最多 64 条） */
        int batch_n = 0;
        while (g_rd != g_wr && batch_n < 64) {
            memcpy(&batch[batch_n], &g_queue[g_rd], sizeof(log_entry_t));
            g_rd = (g_rd + 1) % QUEUE_SIZE;  /* 环形缓冲区指针推进 */
            batch_n++;
        }
        pthread_mutex_unlock(&g_mtx);

        /* 批量写入日志 */
        for (int i = 0; i < batch_n; i++) {
            /* 生成时间戳 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm tm_buf;
            localtime_r(&ts.tv_sec, &tm_buf);  /* 线程安全的 localtime */

            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

            /* 日志级别缩写：D=Debug, I=Info, W=Warn, E=Error, F=Fatal */
            static const char* levels = "DIWEF";
            char lvl = (batch[i].level >= 0 && batch[i].level <= 4) ? levels[batch[i].level] : '?';

            /* 写入日志文件 */
            if (g_file) {
                fprintf(g_file, "%s.%03ld [%c] %s\n", time_buf, ts.tv_nsec / 1000000L, lvl, batch[i].msg);
                fflush(g_file);  /* 每行都 flush，确保崩溃时可恢复 */
            }
            /* 写入控制台（stderr） */
            if (g_console) {
                fprintf(stderr, "%s.%03ld [%c] %s\n", time_buf, ts.tv_nsec / 1000000L, lvl, batch[i].msg);
            }
        }
    }

    /* ── 排空队列中的残余日志 ── */
    pthread_mutex_lock(&g_mtx);
    while (g_rd != g_wr) {
        log_entry_t* e = &g_queue[g_rd];
        g_rd = (g_rd + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&g_mtx);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_buf;
        localtime_r(&ts.tv_sec, &tm_buf);
        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        if (g_file) {
            fprintf(g_file, "%s.%03ld [%c] %s\n", time_buf, ts.tv_nsec / 1000000L, 'I', e->msg);
            fflush(g_file);
        }

        pthread_mutex_lock(&g_mtx);
    }
    pthread_mutex_unlock(&g_mtx);
    return NULL;
}


/* ==========================================================================
 *  公开 API
 * ========================================================================== */

/*
 * 初始化日志系统
 *
 * @file_path：日志文件路径（如 "log/rk3568_vision.log"）
 * @min_level：最低输出级别（低于此值的日志被丢弃）
 * @console：是否输出到控制台（stderr）
 * @async：是否使用异步写入（推荐开启，避免 I/O 阻塞主线程）
 */
void logger_init(const char* file_path, int min_level, int console, int async) {
    g_min_lvl = min_level;
    g_console = console;
    g_async   = async;

    /* 打开日志文件（追加模式） */
    if (file_path && file_path[0]) {
        ensure_log_dir(file_path);
        g_file = fopen(file_path, "a");
    }

    /* 如果开启异步模式，启动写线程 */
    if (async) {
        g_running = 1;
        pthread_create(&g_thread, NULL, writer_thread, NULL);
    }
}

/*
 * 关闭日志系统
 *
 * 流程：
 *   1. 设置 g_running=0（通知写线程退出）
 *   2. 唤醒可能正在等待的写线程（pthread_cond_signal）
 *   3. 等待写线程结束（pthread_join）
 *   4. 关闭日志文件
 */
void logger_shutdown(void) {
    if (!g_running) goto cleanup;
    g_running = 0;
    pthread_cond_signal(&g_cv);    /* 唤醒写线程（可能正在 cond_timedwait） */
    pthread_join(g_thread, NULL);   /* 等待写线程退出 */
cleanup:
    if (g_file) { fclose(g_file); g_file = NULL; }
}

/*
 * 写入一条日志
 *
 * 这是所有 LOG_* 宏的底层实现
 * 流程：
 *   1. 级别过滤（低于 min_level 直接丢弃）
 *   2. 格式化日志消息（[级别][文件名:行号] 消息内容）
 *   3. 同步模式：直接写文件和终端
 *   4. 异步模式：入队 → 唤醒写线程
 *
 * @level：日志级别
 * @file：源文件名（__FILE__）
 * @line：行号（__LINE__）
 * @fmt：printf 格式字符串
 * @...：可变参数
 */
void logger_write(int level, const char* file, int line, const char* fmt, ...) {
    if (level < g_min_lvl) return;  /* 级别过滤 */

    static const char* lvl_str[] = {"D", "I", "W", "E", "F"};
    const char* ls = (level >= 0 && level <= 4) ? lvl_str[level] : "?";

    /* 从完整路径中提取文件名（只取最后一个 / 之后的部分） */
    const char* fname = strrchr(file, '/');
    fname = fname ? fname + 1 : file;

    /* 格式化日志内容：[级别][文件名:行号] 消息 */
    log_entry_t entry;
    entry.level = level;
    int off = snprintf(entry.msg, MSG_MAX, "[%s][%s:%d] ", ls, fname, line);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entry.msg + off, MSG_MAX - off, fmt, ap);
    va_end(ap);

    if (!g_async || !g_running) {
        /* 同步模式：直接写入（不需要队列和锁） */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_buf;
        localtime_r(&ts.tv_sec, &tm_buf);
        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        if (g_file) {
            fprintf(g_file, "%s.%03ld [%s] %s\n", time_buf, ts.tv_nsec / 1000000L, ls, entry.msg);
            fflush(g_file);
        }
        if (g_console) {
            fprintf(stderr, "%s.%03ld [%s] %s\n", time_buf, ts.tv_nsec / 1000000L, ls, entry.msg);
        }
        return;
    }

    /* 异步模式：入队 */
    pthread_mutex_lock(&g_mtx);
    int next_wr = (g_wr + 1) % QUEUE_SIZE;
    if (next_wr != g_rd) {
        /* 队列未满：写入 */
        memcpy(&g_queue[g_wr], &entry, sizeof(entry));
        g_wr = next_wr;
        pthread_cond_signal(&g_cv);  /* 唤醒写线程 */
    } else {
        /* 队列已满：丢弃此条日志（避免阻塞业务线程） */
        static int drop_cnt = 0;
        if (++drop_cnt % 1000 == 0) {
            fprintf(stderr, "[LOGGER] queue full, dropped %d\n", drop_cnt);
        }
    }
    pthread_mutex_unlock(&g_mtx);
}
