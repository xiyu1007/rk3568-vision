/*
 * ==========================================================================
 * monitor.h — 系统资源监控模块头文件
 * ==========================================================================
 *
 * **系统监控**：
 *   后台线程每 2 秒采样一次 CPU 使用率、内存使用率、SoC 温度
 *   数据可用于性能分析和异常告警
 */

#ifndef MONITOR_H
#define MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* 启动监控线程（后台运行） */
void monitor_start(void);

/* 停止监控线程 */
void monitor_stop(void);

/* 获取最新 CPU 利用率（百分比 0-100） */
float monitor_cpu(void);

/* 获取最新内存利用率（百分比 0-100） */
float monitor_mem(void);

/* 获取最新 SoC 温度（摄氏度） */
float monitor_temp(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_H */
