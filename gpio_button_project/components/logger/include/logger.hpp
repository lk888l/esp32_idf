#pragma once

#if __has_include(<format>)
#include <format>
#define LOGGER_HAS_STD_FORMAT 1
#else
#include <cstdio>
#define LOGGER_HAS_STD_FORMAT 0
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ium {

enum class LogLevel : uint8_t {
    NONE = 0,
    ERROR,
    WARN,
    INFO,
    DEBUG,
    VERBOSE,
};

struct LogRecord {
    LogLevel level;
    std::string_view tag;
    std::string_view message;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void write(const LogRecord& record) = 0;
    virtual int writeRaw(const char* data, size_t size);
};

class ConsoleLogSink final : public ILogSink {
public:
    void write(const LogRecord& record) override;
    int writeRaw(const char* data, size_t size) override;
};

class Logger {
public:
    explicit Logger(std::string tag) : tag_(std::move(tag)) {}

    void log(LogLevel level, std::string_view message);

    template<typename... Args>
    void error(std::string_view fmt, Args&&... args);

    template<typename... Args>
    void warn(std::string_view fmt, Args&&... args);

    template<typename... Args>
    void info(std::string_view fmt, Args&&... args);

    template<typename... Args>
    void debug(std::string_view fmt, Args&&... args);

    template<typename... Args>
    void verbose(std::string_view fmt, Args&&... args);

    static void setSink(ILogSink* sink);
    static void resetSink();
    static ILogSink& sink();

    static void setGlobalLevel(LogLevel level) { s_global_level_.store(level, std::memory_order_relaxed); }
    static LogLevel globalLevel() { return s_global_level_.load(std::memory_order_relaxed); }

    static bool shouldLog(LogLevel level);
    static const char* levelName(LogLevel level);
    static const char* levelChar(LogLevel level);

    static int vprintfHook(const char* fmt, va_list args);

private:
    void emit(LogLevel level, std::string_view message);

    template<typename... Args>
    void logFormatted(LogLevel level, std::string_view fmt, Args&&... args);

    template<typename... Args>
    static std::string formatMessage(std::string_view fmt, Args&&... args);

    std::string tag_;

    static std::atomic<LogLevel> s_global_level_;
    static ILogSink* s_sink_;
    static ConsoleLogSink s_default_sink_;
    static std::mutex s_write_mutex_;
};

#if LOGGER_HAS_STD_FORMAT

template<typename... Args>
std::string Logger::formatMessage(std::string_view fmt, Args&&... args)
{
    return std::vformat(fmt, std::make_format_args(args...));
}

#else

template<typename... Args>
std::string Logger::formatMessage(std::string_view fmt, Args&&...)
{
    return std::string(fmt);
}

#endif

template<typename... Args>
void Logger::logFormatted(LogLevel level, std::string_view fmt, Args&&... args)
{
    if (!shouldLog(level)) {
        return;
    }

    if constexpr (sizeof...(Args) == 0) {
        emit(level, fmt);
    } else {
#if LOGGER_HAS_STD_FORMAT
        emit(level, formatMessage(fmt, std::forward<Args>(args)...));
#else
        emit(level, fmt);
#endif
    }
}

template<typename... Args>
void Logger::error(std::string_view fmt, Args&&... args)
{
    logFormatted(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::warn(std::string_view fmt, Args&&... args)
{
    logFormatted(LogLevel::WARN, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::info(std::string_view fmt, Args&&... args)
{
    logFormatted(LogLevel::INFO, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::debug(std::string_view fmt, Args&&... args)
{
    logFormatted(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void Logger::verbose(std::string_view fmt, Args&&... args)
{
    logFormatted(LogLevel::VERBOSE, fmt, std::forward<Args>(args)...);
}

} // namespace ium

namespace espidf_template = ium;
