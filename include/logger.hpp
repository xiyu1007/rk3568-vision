// ============================================================================
// logger.hpp — 分级日志系统（单例 + 异步写 + 文件轮转）
// ============================================================================
//
// 设计要点：
//   1. 【单例】Logger::instance() 全局唯一，各模块直接通过宏调用，无需传参。
//   2. 【分级】Debug / Info / Warn / Error 四级，低于当前 level 的日志被丢弃。
//   3. 【异步】async=true 时，log() 只把格式化好的字符串塞进内存队列，
//      由后台写线程批量落盘/输出，日志 I/O 不阻塞业务线程。
//   4. 【轮转】文件超过 max_size 时重命名为 .1、.2 ...，防止日志无限增长。
//   5. 【时间戳】每条日志带 [HH:MM:SS.mmm] 墙钟时间，便于对照系统行为。
//
// 用法：
//   vision::Logger::instance().init(cfg.log);
//   LOG_INFO("capture started, %dx%d@%dfps", w, h, fps);
//   ...
//   vision::Logger::instance().shutdown();
// ============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common.hpp"   // 使用 LogConfig

namespace vision {

// 日志级别（数值越大越严重）。
enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

class Logger {
public:
    // 全局唯一实例（Meyers 单例，线程安全）。
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 用配置初始化日志系统（启动写线程、打开文件）。
    // 多次调用幂等：第二次调用会忽略（如需重配请先 shutdown）。
    bool init(const LogConfig& cfg);

    // 关闭日志系统：唤醒写线程、排空队列、关闭文件。
    void shutdown();

    // 核心接口：按级别输出一条日志（printf 风格可变参数）。
    void log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))   // 让编译器检查格式串
#endif
        ;

    // 便捷接口。
    void debug(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void info(const char* fmt, ...)  __attribute__((format(printf, 2, 3)));
    void warn(const char* fmt, ...)  __attribute__((format(printf, 2, 3)));
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    // 当前级别（供外部判断是否需要做昂贵计算）。
    LogLevel level() const { return level_; }

private:
    Logger() = default;
    ~Logger();

    // 后台写线程主循环。
    void workerLoop();

    // 真正把一行写进文件/控制台。
    void writeLine(const std::string& line);

    // 若文件超过 max_size 则轮转（重命名 .1/.2...）。
    void rotateIfNeededLocked();

    // 把 level 转成字符串。
    static const char* levelName(LogLevel level);

    // 格式化当前墙钟时间为 "HH:MM:SS.mmm"。
    static std::string formatNow();

    // ---- 配置 ----
    LogLevel    level_   = LogLevel::Info;
    std::string file_;
    bool        console_ = true;
    bool        async_   = true;
    uint64_t    max_size_ = 10ull * 1024 * 1024;
    uint32_t    backup_count_ = 3;

    // ---- 异步写队列 ----
    std::deque<std::string> queue_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             worker_;
    std::atomic<bool>       running_{false};

    // ---- 文件 ----
    FILE* fp_ = nullptr;
    bool  inited_ = false;
    std::mutex file_mutex_;   // 保护文件写与轮转（同步模式与写线程共用）
};

} // namespace vision

// ---------------------------------------------------------------------------
// 便捷宏（业务代码直接用这些宏，无需关心单例细节）
// ---------------------------------------------------------------------------
#define LOG_DEBUG(...) vision::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  vision::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)  vision::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) vision::Logger::instance().error(__VA_ARGS__)

