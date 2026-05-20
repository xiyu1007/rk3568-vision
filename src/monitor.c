/*
 * ==========================================================================
 * monitor.c — 系统资源监控模块
 * ==========================================================================
 *
 * **功能**：定期采集 CPU 使用率、内存使用率、SoC 温度
 *
 * **数据来源**：
 *   - CPU 使用率：/proc/stat（内核 CPU 时间统计）
 *   - 内存使用率：/proc/meminfo（内核内存统计）
 *   - SoC 温度：/sys/class/thermal/thermal_zone1/temp（芯片内部温度传感器）
 *
 * **CPU 利用率计算**：
 *   /proc/stat 第一行包含：cpu user nice system idle iowait irq softirq...
 *   利用率 = 100% - (idle 差值 / total 差值) × 100%
 *   差值 = 当前采样值 - 上次采样值（两次采样的增量）
 *   采样间隔 = 2 秒（sleep(2)），使测量更稳定
 *
 * **内存利用率计算**：
 *   /proc/meminfo 包含：MemTotal, MemAvailable, MemFree...
 *   利用率 = 100% - (MemAvailable / MemTotal) × 100%
 *   MemAvailable 比 MemFree 更准确（包含可回收的缓存）
 *
 * **温度读取**：
 *   /sys/class/thermal/thermal_zone1/temp 文件内容为毫摄氏度（milli°C）
 *   读出的值 ÷ 1000 = 摄氏度
 *   thermal_zone1 通常是 SoC 的 CPU/GPU 温度
 *
 * **监控线程**：
 *   独立后台线程，每 2 秒采样一次
 *   收集的数据通过 getter 函数（monitor_cpu/monitor_mem/monitor_temp）读取
 */

#include "monitor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static pthread_t g_mon_thread;        /* 监控线程句柄 */
static volatile int g_mon_running;    /* 监控线程运行标志 */
static float g_cpu, g_mem, g_temp;    /* 最新采样值 */

/*
 * 监控线程主循环
 *
 * 每 2 秒采样一次系统状态：
 *   1. 读取 /proc/stat → 计算 CPU 利用率
 *   2. 读取 /proc/meminfo → 计算内存利用率
 *   3. 读取 /sys/class/thermal/thermal_zone1/temp → 获取 SoC 温度
 */
static void* mon_run(void* arg) {
    (void)arg;
    float prev_idle = 0, prev_total = 0;  /* 上次 CPU 采样的值 */

    while (g_mon_running) {
        /*
         * CPU 利用率计算
         * 读取 /proc/stat 中的 cpu 行（所有核心汇总）
         * 格式：cpu  user nice system idle iowait ...
         */
        FILE* f = fopen("/proc/stat", "r");
        if (f) {
            char cpu[8];
            float user, nice, sys, idle;
            if (fscanf(f, "%7s %f %f %f %f", cpu, &user, &nice, &sys, &idle) == 5) {
                float total = user + nice + sys + idle;
                if (prev_total > 0 && total > prev_total)
                    /* 利用率 = 100% - 空闲时间占比 */
                    g_cpu = 100.0f * (1.0f - (idle - prev_idle) / (total - prev_total));
                prev_idle = idle;
                prev_total = total;
            }
            fclose(f);
        }

        /*
         * 内存利用率计算
         * 读取 /proc/meminfo 中的 MemTotal 和 MemAvailable
         */
        f = fopen("/proc/meminfo", "r");
        if (f) {
            float mem_total = 0, mem_avail = 0;
            char label[32], unit[8];
            for (int i = 0; i < 10; i++) {
                float val = 0;
                if (fscanf(f, "%31s %f %7s", label, &val, unit) < 2) break;
                if (strcmp(label, "MemTotal:") == 0) mem_total = val;
                if (strcmp(label, "MemAvailable:") == 0) mem_avail = val;
            }
            if (mem_total > 0) g_mem = 100.0f * (1.0f - mem_avail / mem_total);
            fclose(f);
        }

        /*
         * SoC 温度读取
         * thermal_zone0 通常是 CPU, thermal_zone1 通常是 GPU
         * 值单位是毫摄氏度（milli°C），除以 1000 得到摄氏度
         */
        f = fopen("/sys/class/thermal/thermal_zone1/temp", "r");
        if (f) {
            int raw;
            if (fscanf(f, "%d", &raw) == 1) g_temp = raw / 1000.0f;
            fclose(f);
        }

        sleep(2);  /* 采样间隔 2 秒 */
    }
    return NULL;
}

/* 启动监控线程 */
void monitor_start(void) {
    if (g_mon_running) return;   /* 防止重复启动 */
    g_mon_running = 1;
    pthread_create(&g_mon_thread, NULL, mon_run, NULL);
}

/* 停止监控线程 */
void monitor_stop(void) {
    g_mon_running = 0;
    pthread_join(g_mon_thread, NULL);
}

/* 获取最新 CPU 利用率（百分比，0-100） */
float monitor_cpu(void)  { return g_cpu; }

/* 获取最新内存利用率（百分比，0-100） */
float monitor_mem(void)  { return g_mem; }

/* 获取最新 SoC 温度（摄氏度） */
float monitor_temp(void) { return g_temp; }
