// ============================================================================
// debug.hpp — DEBUG 条件编译宏
// ============================================================================
//
// 通过 VISION_DEBUG 宏控制调试信息的开启/关闭（Makefile 的 debug/release 目标）：
//   - make debug   → 定义 VISION_DEBUG，开启性能分析、队列深度、帧生命周期、详细日志
//   - make release → 不定义，全部调试代码被编译器优化掉，零运行时开销
//
// 提供的宏：
//   - VISION_DEBUG_LOG(...)       调试日志（仅 DEBUG 构建输出）
//   - VISION_PROFILE_SCOPE(name)  RAII 作用域计时：进入记录时间戳，退出打印耗时
// ============================================================================

#pragma once

#include "logger.hpp"
#include "types.hpp"

namespace vision {

// 作用域计时器（RAII）：构造时记录起始时间，析构时打印本作用域耗时。
// 用法：
//   void SomeFunction() {
//       VISION_PROFILE_SCOPE("SomeFunction");   // 函数退出时打印耗时
//       ...
//   }
class ScopedProfiler {
public:
    explicit ScopedProfiler(const char* name)
        : name_(name), start_us_(GetCurrentTimestampUs()) {}

    ~ScopedProfiler() {
        const TimestampUs elapsed_us = GetCurrentTimestampUs() - start_us_;
        Logger::instance().debug("[profile] %s took %llu us",
                                 name_,
                                 static_cast<unsigned long long>(elapsed_us));
    }

    ScopedProfiler(const ScopedProfiler&) = delete;
    ScopedProfiler& operator=(const ScopedProfiler&) = delete;

private:
    const char* name_;          // 计时段名称
    TimestampUs start_us_;      // 起始时间戳
};

} // namespace vision

// ---------------------------------------------------------------------------
// 宏定义（依据 VISION_DEBUG 是否定义）
// ---------------------------------------------------------------------------
#ifdef VISION_DEBUG

// DEBUG 构建：开启调试日志与作用域计时。
#define VISION_DEBUG_LOG(...) vision::Logger::instance().debug(__VA_ARGS__)
#define VISION_PROFILE_SCOPE(name) \
    vision::ScopedProfiler _vision_profiler_##__LINE__(name)

#else

// RELEASE 构建：调试代码编译为空操作，零开销。
#define VISION_DEBUG_LOG(...) ((void)0)
#define VISION_PROFILE_SCOPE(name) ((void)0)

#endif
