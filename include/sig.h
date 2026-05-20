/*
 * ==========================================================================
 * sig.h — 信号处理模块头文件
 * ==========================================================================
 *
 * **信号处理**：
 *   捕获 SIGINT（Ctrl+C）和 SIGTERM（kill），实现优雅退出
 *   忽略 SIGPIPE（防止 RTMP 断连时进程崩溃）
 */

#ifndef SIG_H
#define SIG_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 注册信号处理器（SIGINT, SIGTERM, SIGPIPE） */
void sig_setup(void);

/* 检查是否收到关闭信号（非阻塞，各线程主循环中调用） */
int  sig_shutdown(void);

/* 主动触发关闭（正常退出流程中调用） */
void sig_request_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SIG_H */
