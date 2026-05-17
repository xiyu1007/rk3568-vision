#include "monitor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static pthread_t g_mon_thread;
static volatile int g_mon_running;
static float g_cpu, g_mem, g_temp;

static void* mon_run(void* arg) {
    (void)arg;
    float prev_idle = 0, prev_total = 0;

    while (g_mon_running) {
        FILE* f = fopen("/proc/stat", "r");
        if (f) {
            char cpu[8];
            float user, nice, sys, idle;
            if (fscanf(f, "%7s %f %f %f %f", cpu, &user, &nice, &sys, &idle) == 5) {
                float total = user + nice + sys + idle;
                if (prev_total > 0 && total > prev_total)
                    g_cpu = 100.0f * (1.0f - (idle - prev_idle) / (total - prev_total));
                prev_idle = idle;
                prev_total = total;
            }
            fclose(f);
        }

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

        f = fopen("/sys/class/thermal/thermal_zone1/temp", "r");
        if (f) {
            int raw;
            if (fscanf(f, "%d", &raw) == 1) g_temp = raw / 1000.0f;
            fclose(f);
        }

        sleep(2);
    }
    return NULL;
}

void monitor_start(void) {
    if (g_mon_running) return;
    g_mon_running = 1;
    pthread_create(&g_mon_thread, NULL, mon_run, NULL);
}

void monitor_stop(void) {
    g_mon_running = 0;
    pthread_join(g_mon_thread, NULL);
}

float monitor_cpu(void)  { return g_cpu; }
float monitor_mem(void)  { return g_mem; }
float monitor_temp(void) { return g_temp; }
