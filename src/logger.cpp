#include "logger.hpp"

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <iomanip>

namespace rk3568_vision {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() { shutdown(); }

void Logger::init(const std::string& file_path, LogLevel min_level,
                  bool console, bool async, size_t max_size, uint32_t backup) {
    file_path_   = file_path;
    min_level_   = min_level;
    console_     = console;
    async_       = async;
    max_size_    = max_size;
    backup_count_ = backup;

    // 创建日志目录
    auto pos = file_path.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = file_path.substr(0, pos);
        mkdir(dir.c_str(), 0755);
    }

    if (async) {
        running_ = true;
        writer_thread_ = std::thread(&Logger::writer_thread_func, this);
    } else {
        file_stream_.open(file_path, std::ios::app);
    }
}

void Logger::shutdown() {
    if (!running_.exchange(false)) return;

    queue_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

void Logger::log(LogLevel level, const char* file, int line,
                 const char* fmt, ...) {
    if (!should_log(level)) return;

    LogEntry entry;
    entry.level     = level;
    entry.timestamp = now();

    // 格式化消息（文件名:行号 + 用户消息）
    const char* level_str = "?";
    switch (level) {
        case LogLevel::DEBUG: level_str = "D"; break;
        case LogLevel::INFO:  level_str = "I"; break;
        case LogLevel::WARN:  level_str = "W"; break;
        case LogLevel::ERROR: level_str = "E"; break;
        case LogLevel::FATAL: level_str = "F"; break;
    }

    int offset = snprintf(entry.message, sizeof(entry.message),
                          "[%s][%s:%d] ", level_str, file, line);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entry.message + offset, sizeof(entry.message) - offset, fmt, ap);
    va_end(ap);

    if (async_) {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (log_queue_.size() < MAX_QUEUE_SIZE) {
            log_queue_.push(std::move(entry));
        }
        queue_cv_.notify_one();
    } else {
        write_entry(entry);
    }
}

void Logger::writer_thread_func() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mtx_);
        queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return !log_queue_.empty() || !running_; });

        // 批量处理队列中的所有日志
        while (!log_queue_.empty()) {
            auto& entry = log_queue_.front();
            write_entry(entry);
            log_queue_.pop();
        }

        if (!running_ && log_queue_.empty()) break;
    }
}

void Logger::write_entry(const LogEntry& entry) {
    // 格式化时间戳
    auto ms = timestamp_to_ms(entry.timestamp);
    time_t sec = ms / 1000;
    int ms_part = ms % 1000;
    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // 写入文件
    if (file_stream_.is_open()) {
        file_stream_ << time_buf << "." << std::setfill('0') << std::setw(3)
                     << ms_part << " " << entry.message << std::endl;
    }

    // 控制台输出
    if (console_) {
        std::cout << time_buf << "." << std::setfill('0') << std::setw(3)
                  << ms_part << " " << entry.message << std::endl;
    }
}

} // namespace rk3568_vision
