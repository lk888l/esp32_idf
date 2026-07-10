#include "logger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace ium {

std::atomic<LogLevel> Logger::s_global_level_{LogLevel::INFO};
ConsoleLogSink Logger::s_default_sink_;
ILogSink* Logger::s_sink_ = &Logger::s_default_sink_;
std::mutex Logger::s_write_mutex_;

int ILogSink::writeRaw(const char* data, size_t size)
{
    LogRecord record{
        .level = LogLevel::INFO,
        .tag = "raw",
        .message = std::string_view(data, size),
    };
    write(record);
    return static_cast<int>(size);
}

void ConsoleLogSink::write(const LogRecord& record)
{
    std::printf("[%s][%.*s] %.*s\n",
                Logger::levelChar(record.level),
                static_cast<int>(record.tag.size()),
                record.tag.data(),
                static_cast<int>(record.message.size()),
                record.message.data());
}

int ConsoleLogSink::writeRaw(const char* data, size_t size)
{
    return static_cast<int>(std::fwrite(data, 1, size, stdout));
}

void Logger::log(LogLevel level, std::string_view message)
{
    if (!shouldLog(level)) {
        return;
    }

    emit(level, message);
}

void Logger::emit(LogLevel level, std::string_view message)
{
    LogRecord record{
        .level = level,
        .tag = tag_,
        .message = message,
    };

    std::lock_guard<std::mutex> lock(s_write_mutex_);
    s_sink_->write(record);
}

void Logger::setSink(ILogSink* sink)
{
    std::lock_guard<std::mutex> lock(s_write_mutex_);
    s_sink_ = sink ? sink : &s_default_sink_;
}

void Logger::resetSink()
{
    setSink(nullptr);
}

ILogSink& Logger::sink()
{
    return *s_sink_;
}

bool Logger::shouldLog(LogLevel level)
{
    const LogLevel global_level = s_global_level_.load(std::memory_order_relaxed);
    return global_level != LogLevel::NONE
        && level != LogLevel::NONE
        && static_cast<uint8_t>(level) <= static_cast<uint8_t>(global_level);
}

const char* Logger::levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::WARN: return "WARN";
    case LogLevel::INFO: return "INFO";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::VERBOSE: return "VERBOSE";
    default: return "NONE";
    }
}

const char* Logger::levelChar(LogLevel level)
{
    switch (level) {
    case LogLevel::ERROR: return "E";
    case LogLevel::WARN: return "W";
    case LogLevel::INFO: return "I";
    case LogLevel::DEBUG: return "D";
    case LogLevel::VERBOSE: return "V";
    default: return "-";
    }
}

int Logger::vprintfHook(const char* fmt, va_list args)
{
    std::array<char, 256> buffer{};
    va_list args_copy;
    va_copy(args_copy, args);
    int len = std::vsnprintf(buffer.data(), buffer.size(), fmt, args_copy);
    va_end(args_copy);

    if (len <= 0) {
        return len;
    }

    size_t bytes = std::min(static_cast<size_t>(len), buffer.size() - 1);
    std::lock_guard<std::mutex> lock(s_write_mutex_);
    s_sink_->writeRaw(buffer.data(), bytes);
    return len;
}

} // namespace ium
