/*
 * ==========================================================================
 * fps.c — 实时帧率统计模块
 * ==========================================================================
 *
 * **实现原理**：
 *   使用滑动窗口法统计帧率
 *   在每次 fps_tick() 调用时计数，每 500ms 计算一次实时帧率
 *
 * **为什么选择 500ms 窗口？**
 *   - 太短（如 100ms）：FPS 数值波动大，显示不稳定（抖动）
 *   - 太长（如 2000ms）：FPS 响应慢，不能反映实时性能变化
 *   - 500ms 是平衡点：足够平滑，又能快速反映帧率变化
 *
 * **FPS 计算公式**：
 *   fps = count / elapsed（帧数 / 时间间隔秒数）
 *   例如：500ms 内采集了 15 帧 → fps = 15 / 0.5 = 30.0
 *
 * **使用方式**：
 *   // 每处理一帧调用一次
 *   fps_tick();
 *   // 在显示线程中获取当前帧率
 *   double current_fps = fps_get();
 */

#include "fps.h"
#include <time.h>

/* FPS 统计器内部状态（全局单例） */
static struct {
    double    last;       /* 上次计算 FPS 的时间点（秒） */
    long long count;      /* 当前窗口内的帧计数         */
    double    fps;        /* 最近计算的 FPS 值          */
    int       init;       /* 是否已完成首次初始化       */
} g_fps;

/*
 * 记录一帧（由各工作线程在每帧处理后调用）
 *
 * 首次调用时初始化基准时间
 * 每 500ms 计算一次 FPS 并重置计数器
 */
void fps_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  /* 单调时钟，不受系统时间调整影响 */
    double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;  /* 转换为秒（浮点） */

    if (!g_fps.init) {
        /* 首次调用：初始化基准时间 */
        g_fps.last  = now;
        g_fps.count = 0;
        g_fps.init  = 1;
        return;
    }

    g_fps.count++;  /* 帧计数器 +1 */

    double elapsed = now - g_fps.last;
    if (elapsed >= 0.5) {  /* 每 500ms 计算一次 */
        g_fps.fps   = (double)g_fps.count / elapsed;  /* FPS = 帧数 / 时间 */
        g_fps.last  = now;     /* 重置基准时间         */
        g_fps.count = 0;       /* 重置帧计数器         */
    }
}

/* 获取当前 FPS 值（双精度浮点数） */
double fps_get(void) {
    return g_fps.fps;
}

/* 重置 FPS 统计器（初始化状态） */
void fps_reset(void) {
    g_fps.last  = 0.0;
    g_fps.count = 0;
    g_fps.fps   = 0.0;
    g_fps.init  = 0;
}
