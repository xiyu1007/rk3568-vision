/*
 * ==========================================================================
 * fps.h — 帧率统计模块头文件
 * ==========================================================================
 *
 * **FPS 统计**：使用 500ms 滑动窗口计算实时帧率
 *   各工作线程在每帧处理后调用 fps_tick()
 *   显示线程通过 fps_get() 获取当前 FPS 值用于叠加显示
 */

#ifndef FPS_H
#define FPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 记录一帧（帧计数器 +1，每 500ms 计算一次 FPS） */
void   fps_tick(void);

/* 获取当前 FPS 值（双精度浮点） */
double fps_get(void);

/* 重置 FPS 统计器 */
void   fps_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* FPS_H */
