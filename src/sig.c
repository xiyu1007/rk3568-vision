/*
 * ==========================================================================
 * sig.c — 信号处理模块
 * ==========================================================================
 *
 * **功能**：优雅处理 Ctrl+C（SIGINT）和 SIGTERM 信号
 *
 * **为什么需要信号处理？**
 *   - 嵌入式设备通常通过 systemd 或手动 kill 来管理进程
 *   - 如果没有信号处理，SIGTERM 会导致进程立即终止
 *   - 资源（DMA buffer、mmap、RTMP 连接）不会被释放
 *   - 优雅退出确保：释放所有硬件资源、flush 编码缓冲区、关闭网络连接
 *
 * **SIGPIPE 忽略**：
 *   - 当 RTMP 连接断开时，写入已关闭的 socket 会触发 SIGPIPE
 *   - 默认情况下 SIGPIPE 会导致进程终止
 *   - 我们忽略 SIGPIPE，让 write() 返回 -1 错误码
 *   - 由 RTMP 模块检测错误并处理重连逻辑
 *
 * **线程安全**：
 *   volatile sig_atomic_t + 原子读写
 *   虽然这里不是原子操作，但在大多数平台上对 int 的单次读写是原子的
 *   sig_shutdown() 在多线程中只读，sig_handler() 只写，无竞争
 */

#include "sig.h"
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_shutdown = 0;  /* 关闭标志（原子类型） */

/*
 * 信号处理函数
 * 被 OS 在信号到达时调用（在信号上下文中执行）
 * 只做最安全的事情：设置一个全局标志
 * POSIX 规定 signal handler 中只能安全地修改 volatile sig_atomic_t 变量
 */
static void sig_handler(int sig) {
    (void)sig;         /* 忽略信号编号（SIGINT 或 SIGTERM 都同样处理） */
    g_shutdown = 1;    /* 设置关闭标志 */
}

/*
 * 注册信号处理器
 *
 * 注册 SIGINT（Ctrl+C）和 SIGTERM（kill/终止）的处理函数
 * 忽略 SIGPIPE（防止 RTMP 断连时进程崩溃）
 *
 * 使用 sigaction 而非 signal 的原因：
 *   - sigaction 是 POSIX 标准，行为可预测
 *   - signal 在不同 UNIX 变体上行为不一致
 *   - sigaction 提供更多控制（如 SA_RESTART 等标志）
 */
void sig_setup(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;  /* 信号处理函数 */
    sa.sa_flags   = 0;             /* 无特殊标志 */
    sigemptyset(&sa.sa_mask);      /* 不阻塞任何额外信号 不额外屏蔽其它信号*/

    // 向内核注册
    sigaction(SIGINT,  &sa, NULL);   /* Ctrl+C */
    sigaction(SIGTERM, &sa, NULL);   /* kill <pid> / systemd stop */

    /* 忽略 SIGPIPE：防止写入已断开 RTMP 连接时进程终止 */
    // RTMP服务器断开
    // socket 已关闭
    // SIGPIPE -> 直接杀死进程
    signal(SIGPIPE, SIG_IGN);
}

/* 检查是否收到退出信号（各线程主循环调用此函数判断是否退出） */
int sig_shutdown(void) {
    return g_shutdown;
}

/* 主动请求关闭（pipeline_stop 中调用） */
void sig_request_shutdown(void) {
    g_shutdown = 1;
}
