#include "logger.hpp"
#include "uart_driver.hpp"          // IUartDriver
#include "esp_log.h"                // esp_log_set_vprintf
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace espidf_template {

// ── static member definitions ──────────────────────────────────
IUartDriver* Logger::s_uart_          = nullptr;
LogLevel     Logger::s_global_level_  = LogLevel::INFO;
std::mutex   Logger::s_write_mutex_;

// ── helpers ────────────────────────────────────────────────────

const char* Logger::levelChar(LogLevel lv)
{
    switch (lv) {
    case LogLevel::ERROR:   return "E";
    case LogLevel::WARN:    return "W";
    case LogLevel::INFO:    return "I";
    case LogLevel::DEBUG:   return "D";
    case LogLevel::VERBOSE: return "V";
    default:                return "?";
    }
}

// ── static setup ───────────────────────────────────────────────

void Logger::setUart(IUartDriver* uart)
{
    std::lock_guard<std::mutex> lock(s_write_mutex_);
    s_uart_ = uart;
}

// ── output core ────────────────────────────────────────────────

void Logger::writeImpl(LogLevel level, std::string_view msg)
{
    std::lock_guard<std::mutex> lock(s_write_mutex_);
    if (!s_uart_) return;

    // Build:  [L][tag] payload\r\n
    char hdr[96];
    int  hdr_len = snprintf(hdr, sizeof(hdr), "[%s][%s] ",
                            levelChar(level), tag_.c_str());
    if (hdr_len > 0) {
        s_uart_->write(reinterpret_cast<const uint8_t*>(hdr),
                       static_cast<size_t>(hdr_len));
    }
    s_uart_->write(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    s_uart_->write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
}

// ── vprintf hook (for esp_log_set_vprintf) ─────────────────────

int Logger::vprintfHook(const char* fmt, va_list args)
{
    // No lock here — esp_log_writev already serialises upstream.
    // s_uart_ is treated as read-only after setUart() at boot, so this
    // is safe without a mutex on the hot path.

    char buf[256];                     // stack buffer — no heap allocation
    int  len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) return 0;

    size_t n = std::min(static_cast<size_t>(len), sizeof(buf) - 1);

    if (s_uart_) {
        s_uart_->write(reinterpret_cast<const uint8_t*>(buf), n);
    }
    return len;
}

} // namespace espidf_template
