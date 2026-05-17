#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MSG_MAX     512
#define QUEUE_SIZE  4096

typedef struct {
    int   level;
    char  msg[MSG_MAX];
} log_entry_t;

/* ── Thread-safe circular buffer ──────────────────────────────────────── */
static log_entry_t g_queue[QUEUE_SIZE];
static volatile int g_wr = 0;
static volatile int g_rd = 0;

static pthread_mutex_t  g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t        g_thread;
static volatile int     g_running = 0;

static FILE*      g_file    = NULL;
static int        g_min_lvl = LOG_LEVEL_INFO;
static int        g_console = 1;
static int        g_async   = 1;

/* ── File rotation helpers ────────────────────────────────────────────── */
static void ensure_log_dir(const char* path) {
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
}

/* ── Writer thread ────────────────────────────────────────────────────── */
static void* writer_thread(void* arg) {
    (void)arg;
    log_entry_t batch[64];

    while (g_running) {
        pthread_mutex_lock(&g_mtx);
        while (g_rd == g_wr && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000; /* 100ms */
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
        }

        int batch_n = 0;
        while (g_rd != g_wr && batch_n < 64) {
            memcpy(&batch[batch_n], &g_queue[g_rd], sizeof(log_entry_t));
            g_rd = (g_rd + 1) % QUEUE_SIZE;
            batch_n++;
        }
        pthread_mutex_unlock(&g_mtx);

        for (int i = 0; i < batch_n; i++) {
            /* timestamp */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm tm_buf;
            localtime_r(&ts.tv_sec, &tm_buf);

            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

            static const char* levels = "DIWEF";
            char lvl = (batch[i].level >= 0 && batch[i].level <= 4) ? levels[batch[i].level] : '?';

            if (g_file) {
                fprintf(g_file, "%s.%03ld [%c] %s\n", time_buf, ts.tv_nsec / 1000000L, lvl, batch[i].msg);
                fflush(g_file);
            }
            if (g_console) {
                fprintf(stderr, "%s.%03ld [%c] %s\n", time_buf, ts.tv_nsec / 1000000L, lvl, batch[i].msg);
            }
        }
    }

    /* drain remaining */
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

/* ── Public API ───────────────────────────────────────────────────────── */

void logger_init(const char* file_path, int min_level, int console, int async) {
    g_min_lvl = min_level;
    g_console = console;
    g_async   = async;

    if (file_path && file_path[0]) {
        ensure_log_dir(file_path);
        g_file = fopen(file_path, "a");
    }

    if (async) {
        g_running = 1;
        pthread_create(&g_thread, NULL, writer_thread, NULL);
    }
}

void logger_shutdown(void) {
    if (!g_running) goto cleanup;
    g_running = 0;
    pthread_cond_signal(&g_cv);
    pthread_join(g_thread, NULL);
cleanup:
    if (g_file) { fclose(g_file); g_file = NULL; }
}

void logger_write(int level, const char* file, int line, const char* fmt, ...) {
    if (level < g_min_lvl) return;

    static const char* lvl_str[] = {"D", "I", "W", "E", "F"};
    const char* ls = (level >= 0 && level <= 4) ? lvl_str[level] : "?";

    /* extract filename from path */
    const char* fname = strrchr(file, '/');
    fname = fname ? fname + 1 : file;

    log_entry_t entry;
    entry.level = level;
    int off = snprintf(entry.msg, MSG_MAX, "[%s][%s:%d] ", ls, fname, line);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entry.msg + off, MSG_MAX - off, fmt, ap);
    va_end(ap);

    if (!g_async || !g_running) {
        /* sync: write immediately */
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

    /* async: enqueue */
    pthread_mutex_lock(&g_mtx);
    int next_wr = (g_wr + 1) % QUEUE_SIZE;
    if (next_wr != g_rd) {
        memcpy(&g_queue[g_wr], &entry, sizeof(entry));
        g_wr = next_wr;
        pthread_cond_signal(&g_cv);
    } else {
        static int drop_cnt = 0;
        if (++drop_cnt % 1000 == 0) {
            fprintf(stderr, "[LOGGER] queue full, dropped %d\n", drop_cnt);
        }
    }
    pthread_mutex_unlock(&g_mtx);
}
