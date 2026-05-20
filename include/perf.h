/*
 * ==========================================================================
 * perf.h — 性能统计模块头文件
 * ==========================================================================
 *
 * **全局性能计数器 g_perf**：
 *   记录各处理阶段的延迟和累计帧数/丢帧数
 *   使用原子操作保证多线程安全
 */

#ifndef PERF_H
#define PERF_H

#include "types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern perf_t g_perf;  /* 全局性能计数器 */

void perf_record_capture(int64_t us);  /* 记录采集延迟（微秒） */
void perf_record_infer(int64_t us);    /* 记录推理延迟（微秒） */
void perf_record_encode(int64_t us);   /* 记录编码延迟（微秒） */
void perf_record_drop(void);           /* 记录一次丢帧         */
void perf_report(void);                /* 输出性能报告         */

#ifdef __cplusplus
}
#endif

#endif /* PERF_H */
