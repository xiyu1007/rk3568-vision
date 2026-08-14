// ============================================================================
// logger.cpp — 日志系统实现
// ============================================================================

#include "logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace vision {

namespace {
// 单条日志格式化缓冲的最大长度（超出会被截断，够用即可）。
constexpr size_t kMaxLineLen = 2048;
} // namespace

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
Logger& Logger::instance() {
    static Logger inst;   // 局部静态对象，首次调用时构造，线程安全
    return inst;
}

Logger::~Logger() {
    shutdown();
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool Logger::init(const LogConfig& cfg) {
    // 若已初始化则直接返回（幂等），避免重复启动写线程。
    if (inited_) return true;

    // 解析日志级别字符串。
    if (cfg.level == "debug")      level_ = LogLevel::Debug;
    else if (cfg.level == "warn")  level_ = LogLevel::Warn;
    else if (cfg.level == "error") level_ = LogLevel::Error;
    else                           level_ = LogLevel::Info;

    file_         = cfg.file;
    console_      = cfg.console;
    async_        = cfg.async;
    max_size_     = cfg.max_size;
    backup_count_ = cfg.backup_count;

    // 打开日志文件（追加模式）。目录不存在则先创建。
    if (!file_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(file_).parent_path(), ec);
        fp_ = std::fopen(file_.c_str(), "a");
    }

    inited_ = true;

    // 异步模式：启动后台写线程。
    if (async_) {
        running_.store(true);
        worker_ = std::thread(&Logger::workerLoop, this);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------
void Logger::shutdown() {
    if (!inited_) return;

    if (async_) {
        running_.store(false);
        cv_.notify_all();              // 唤醒写线程，让它退出
        if (worker_.joinable()) worker_.join();
    }

    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    inited_ = false;
}

// ---------------------------------------------------------------------------
// 日志级别名称
// ---------------------------------------------------------------------------
const char* Logger::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

// ---------------------------------------------------------------------------
// 当前墙钟时间格式化
// ---------------------------------------------------------------------------
std::string Logger::formatNow() {
    // 取系统时间（墙钟），精确到毫秒。
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count());
    return buf;
}

// ---------------------------------------------------------------------------
// 核心日志接口
// ---------------------------------------------------------------------------
void Logger::log(LogLevel level, const char* fmt, ...) {
    // 低于当前级别的日志直接丢弃（省去格式化开销）。
    if (static_cast<int>(level) < static_cast<int>(level_)) return;

    // 1. 格式化消息体。
    char body[kMaxLineLen];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    // 2. 拼装完整一行："[HH:MM:SS.mmm] [INFO ] body"。
    std::string line = "[" + formatNow() + "] [" + levelName(level) + "] " + body + "\n";

    if (async_) {
        // 异步：入队，由写线程落盘。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(line));
        }
        cv_.notify_one();
    } else {
        // 同步：直接写。
        std::lock_guard<std::mutex> lock(file_mutex_);
        writeLine(line);
    }
}

void Logger::debug(const char* fmt, ...) {
    if (static_cast<int>(LogLevel::Debug) < static_cast<int>(level_)) return;
    va_list ap; va_start(ap, fmt);
    char body[kMaxLineLen]; std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log(LogLevel::Debug, "%s", body);
}

void Logger::info(const char* fmt, ...) {
    if (static_cast<int>(LogLevel::Info) < static_cast<int>(level_)) return;
    va_list ap; va_start(ap, fmt);
    char body[kMaxLineLen]; std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log(LogLevel::Info, "%s", body);
}

void Logger::warn(const char* fmt, ...) {
    if (static_cast<int>(LogLevel::Warn) < static_cast<int>(level_)) return;
    va_list ap; va_start(ap, fmt);
    char body[kMaxLineLen]; std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log(LogLevel::Warn, "%s", body);
}

void Logger::error(const char* fmt, ...) {
    if (static_cast<int>(LogLevel::Error) < static_cast<int>(level_)) return;
    va_list ap; va_start(ap, fmt);
    char body[kMaxLineLen]; std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log(LogLevel::Error, "%s", body);
}

// ---------------------------------------------------------------------------
// 后台写线程
// ---------------------------------------------------------------------------
void Logger::workerLoop() {
    while (running_.load()) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 等待队列非空或退出信号。
            cv_.wait(lock, [this] { return !queue_.empty() || !running_.load(); });
            // 一次性把当前所有日志批量取出（减少锁竞争与 I/O 次数）。
            batch.swap(queue_);
        }
        // 批量写。
        std::lock_guard<std::mutex> fLock(file_mutex_);
        for (const auto& line : batch) {
            writeLine(line);
        }
    }
    // 退出前清空残留队列，避免丢失最后几条日志。
    std::lock_guard<std::mutex> fLock(file_mutex_);
    while (!queue_.empty()) {
        writeLine(queue_.front());
        queue_.pop_front();
    }
}

// ---------------------------------------------------------------------------
// 写一行（文件 + 控制台）
// ---------------------------------------------------------------------------
void Logger::writeLine(const std::string& line) {
    if (fp_) {
        std::fwrite(line.data(), 1, line.size(), fp_);
        std::fflush(fp_);                    // 立即刷新，崩溃时也能看到
        rotateIfNeededLocked();
    }
    if (console_) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
}

// ---------------------------------------------------------------------------
// 文件轮转
// ---------------------------------------------------------------------------
void Logger::rotateIfNeededLocked() {
    if (!fp_ || max_size_ == 0) return;

    long pos = std::ftell(fp_);
    if (pos < 0 || static_cast<uint64_t>(pos) < max_size_) return;

    // 关闭当前文件，然后重命名 .N -> .N+1，最后重新打开新文件。
    std::fclose(fp_);
    fp_ = nullptr;

    // 删除最旧的一份备份（backup_count 个）。
    std::string oldest = file_ + "." + std::to_string(backup_count_);
    std::remove(oldest.c_str());

    // 依次把 .N 重命名为 .N+1（从后往前，避免覆盖）。
    for (int i = static_cast<int>(backup_count_) - 1; i >= 1; --i) {
        std::string src = file_ + "." + std::to_string(i);
        std::string dst = file_ + "." + std::to_string(i + 1);
        std::rename(src.c_str(), dst.c_str());
    }
    // 当前文件重命名为 .1。
    std::rename(file_.c_str(), (file_ + ".1").c_str());

    // 重新打开新文件。
    fp_ = std::fopen(file_.c_str(), "a");
}

} // namespace vision
