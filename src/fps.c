#include "fps.h"
#include <time.h>

static struct {
    double    last;
    long long count;
    double    fps;
    int       init;
} g_fps;

void fps_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

    if (!g_fps.init) {
        g_fps.last  = now;
        g_fps.count = 0;
        g_fps.init  = 1;
        return;
    }

    g_fps.count++;
    double elapsed = now - g_fps.last;
    if (elapsed >= 0.5) {
        g_fps.fps   = (double)g_fps.count / elapsed;
        g_fps.last  = now;
        g_fps.count = 0;
    }
}

double fps_get(void) {
    return g_fps.fps;
}

void fps_reset(void) {
    g_fps.last  = 0.0;
    g_fps.count = 0;
    g_fps.fps   = 0.0;
    g_fps.init  = 0;
}
