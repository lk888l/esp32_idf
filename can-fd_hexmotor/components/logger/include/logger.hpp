// logger.hpp — std::format-based logger backed by IUartDriver (DMA UART)
#pragma once

// ── Check std::format availability ─────────────────────────────
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

class IUartDriver;  // fwd — from bsp/uart_driver.hpp

namespace espidf_template {

/** Log level — controls the verbosity of Logger output. */
enum class LogLevel : uint8_t {
    NONE    = 0,   ///< Suppress all output
    ERROR   = 1,   ///< Critical errors only
    WARN    = 2,   ///< Warnings
    INFO    = 3,   ///< Informational (default)
    DEBUG   = 4,   ///< Debug-level detail
    VERBOSE = 5,   ///< Maximum verbosity
};

/**
 * @brief std::format-style logger that outputs via a DMA UART driver.
 *
 * Two integration paths:
 *  1. **Direct API** — logger.info("motor {} pos={:.3f}", id, pos);
 *     → std::vformat → UART DMA TX (type-safe, compile-time checked).
 *  2. **ESP_LOGx interception** — esp_log_set_vprintf(&Logger::vprintfHook)
 *     → vsnprintf → UART DMA TX (printf-compat, captures all IDF logs).
 *
 * Thread-safe: all writes are serialised through a static mutex.
 */
class Logger
{
public:
    explicit Logger(std::string tag) : tag_(std::move(tag)) {}

    /// ---- direct API (std::format style, type-safe) ----

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

    /**
     * @brief Set the UART output channel.
     * @note Must be called once at boot, before any logging task starts.
     *       s_uart_ is thereafter treated as read-only on the hot path.
     */
    static void setUart(IUartDriver* uart);

    /** @brief Global log-level filter.  Messages below this level are discarded. */
    static void setGlobalLevel(LogLevel level) { s_global_level_ = level; }

    /**
     * @brief vprintf-compatible hook for esp_log_set_vprintf().
     *
     * Captures all ESP_LOGx output and routes it through the same UART
     * channel.  Uses a stack buffer (no heap allocation on the hot path).
     */
    static int vprintfHook(const char* fmt, va_list args);

private:
    std::string tag_;

    /**
     * @brief Core output: formats [L][tag] prefix + message + \r\n and
     *        writes to the UART.  Serialised by s_write_mutex_.
     */
    void writeImpl(LogLevel level, std::string_view msg);

    /// -- globals --
    static IUartDriver* s_uart_;
    static LogLevel     s_global_level_;
    static std::mutex   s_write_mutex_;

    /** @brief Map log level to single-character prefix (E/W/I/D/V). */
    static const char* levelChar(LogLevel lv);
};

// ============================================================================
//  Template implementations (must be visible at every call site)
// ============================================================================

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

#else  // !LOGGER_HAS_STD_FORMAT — bare fallback (format args discarded)

template<typename... Args>
inline void Logger::error(std::string_view fmt, Args&&...)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::ERROR))
        writeImpl(LogLevel::ERROR, fmt);
}
template<typename... Args>
inline void Logger::warn(std::string_view fmt, Args&&...)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::WARN))
        writeImpl(LogLevel::WARN, fmt);
}
template<typename... Args>
inline void Logger::info(std::string_view fmt, Args&&...)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::INFO))
        writeImpl(LogLevel::INFO, fmt);
}
template<typename... Args>
inline void Logger::debug(std::string_view fmt, Args&&...)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::DEBUG))
        writeImpl(LogLevel::DEBUG, fmt);
}
template<typename... Args>
inline void Logger::verbose(std::string_view fmt, Args&&...)
{
    if (static_cast<uint8_t>(s_global_level_) >= static_cast<uint8_t>(LogLevel::VERBOSE))
        writeImpl(LogLevel::VERBOSE, fmt);
}

#endif // LOGGER_HAS_STD_FORMAT

} // namespace espidf_template
