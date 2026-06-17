// logger.hpp — std::format-based logger backed by IUartDriver (DMA UART)
#pragma once

// ── Check std::format availability ─────────────────────────
#if __has_include(<format>)
    #include <format>
    #define LOGGER_HAS_STD_FORMAT 1
#else
    #include <cstdio>
    #define LOGGER_HAS_STD_FORMAT 0
    #warning "std::format (<format>) not found by toolchain. "
    #warning "Run: idf.py add-dependency fmt   then replace <format> with <fmt/format.h>"
#endif

#include <string>
#include <string_view>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

class IUartDriver;  // fwd – from bsp/uart_driver.hpp

namespace espidf_template {

enum class LogLevel : uint8_t {
    NONE    = 0,
    ERROR   = 1,
    WARN    = 2,
    INFO    = 3,
    DEBUG   = 4,
    VERBOSE = 5,
};

/**
 * @brief std::format-style logger that outputs via a DMA UART driver.
 *
 * Two integration paths:
 *  1. Direct calls – logger.info("motor {} pos={:.3f}", id, pos);
 *     → std::format → UART DMA TX  (type-safe, compile-time checked)
 *  2. ESP_LOGx interception – esp_log_set_vprintf(&Logger::vprintfHook)
 *     → vsnprintf → UART DMA TX  (printf-compat, captures all IDF logs)
 */
class Logger
{
public:
    explicit Logger(std::string tag) : tag_(std::move(tag)) {}

    /// ---- direct API (std::format style) ----

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

    /// ---- static: global setup ----

    /// Set the UART output channel (must be initialised before use).
    static void setUart(IUartDriver* uart);

    /// Global log-level filter.
    static void setGlobalLevel(LogLevel level) { s_global_level_ = level; }

    /// @brief vprintf-compatible hook.  Install with esp_log_set_vprintf().
    static int vprintfHook(const char* fmt, va_list args);

private:
    std::string tag_;

    void writeImpl(LogLevel level, std::string_view msg);

    /// -- globals --
    static IUartDriver* s_uart_;
    static LogLevel     s_global_level_;
    static std::mutex   s_write_mutex_;

    static const char* levelChar(LogLevel lv);
};

// ────────────────────────────────────────────────────────────
//  Template implementations (must be visible at every call site)
// ────────────────────────────────────────────────────────────

#if LOGGER_HAS_STD_FORMAT

template<typename... Args>
void Logger::error(std::string_view fmt, Args&&... args)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::ERROR)) {
        writeImpl(LogLevel::ERROR,
                  std::vformat(fmt, std::make_format_args(args...)));
    }
}

template<typename... Args>
void Logger::warn(std::string_view fmt, Args&&... args)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::WARN)) {
        writeImpl(LogLevel::WARN,
                  std::vformat(fmt, std::make_format_args(args...)));
    }
}

template<typename... Args>
void Logger::info(std::string_view fmt, Args&&... args)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::INFO)) {
        writeImpl(LogLevel::INFO,
                  std::vformat(fmt, std::make_format_args(args...)));
    }
}

template<typename... Args>
void Logger::debug(std::string_view fmt, Args&&... args)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::DEBUG)) {
        writeImpl(LogLevel::DEBUG,
                  std::vformat(fmt, std::make_format_args(args...)));
    }
}

template<typename... Args>
void Logger::verbose(std::string_view fmt, Args&&... args)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::VERBOSE)) {
        writeImpl(LogLevel::VERBOSE,
                  std::vformat(fmt, std::make_format_args(args...)));
    }
}

#else  // !LOGGER_HAS_STD_FORMAT — bare fallback

template<typename... Args>
inline void Logger::error(std::string_view fmt, Args&&...) {
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::ERROR))
        writeImpl(LogLevel::ERROR, fmt);
}
template<typename... Args>
inline void Logger::warn(std::string_view fmt, Args&&...) {
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::WARN))
        writeImpl(LogLevel::WARN, fmt);
}
template<typename... Args>
inline void Logger::info(std::string_view fmt, Args&&...) {
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::INFO))
        writeImpl(LogLevel::INFO, fmt);
}
template<typename... Args>
inline void Logger::debug(std::string_view fmt, Args&&...) {
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::DEBUG))
        writeImpl(LogLevel::DEBUG, fmt);
}
template<typename... Args>
inline void Logger::verbose(std::string_view fmt, Args&&...) {
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::VERBOSE))
        writeImpl(LogLevel::VERBOSE, fmt);
}

#endif // LOGGER_HAS_STD_FORMAT

} // namespace espidf_template
