#include "serial_console.hpp"
#include "debug_probe.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace connectivity {
esp_err_t SerialConsole::start_console(RequestHandler handler, void* context)
{
#if CONFIG_M5_CONNECTIVITY_CONSOLE_ENABLED && !defined(M5_STICKS3_DAP_SMOKE_TEST)
    if (!handler || is_running()) return ESP_ERR_INVALID_STATE;
    handler_ = handler; context_ = context; framer_.reset();
    if (!start()) return ESP_ERR_NO_MEM;
    ESP_LOGI("radio_console", "ready: help | help wifi | help ble | JSON API v1; no input echo");
#else
    (void)handler; (void)context;
#endif
    return ESP_OK;
}
esp_err_t SerialConsole::stop_console()
{
    if (!stop()) return ESP_ERR_TIMEOUT;
    handler_ = nullptr; context_ = nullptr; framer_.reset();
    std::memset(request_, 0, sizeof(request_));
    return ESP_OK;
}
void SerialConsole::handle_line(console::LineFramer::Result result)
{
    if (result == console::LineFramer::Result::none) return;
    if (++sequence_ > 0x3fffffff) sequence_ = 1;
    const char* error = result == console::LineFramer::Result::too_long ? "line_too_long" :
        result == console::LineFramer::Result::invalid_character ? "invalid_line" :
        console::compile(framer_.line(), sequence_, request_, sizeof(request_));
    char* json = output_ + 1;
    if (error) std::snprintf(json, kMaxResponseBytes,
        "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}",
        static_cast<unsigned long>(sequence_), error);
    else handler_(context_, request_, std::strlen(request_), json, kMaxResponseBytes);
    // A complete RS + JSON + LF record is emitted with one stdio call so it
    // shares the console's output lock with ESP-IDF logging. No input echo.
    output_[0] = console::kRecordSeparator;
    const size_t length = strnlen(json, kMaxResponseBytes - 1);
    output_[length + 1] = '\n';
    std::fwrite(output_, 1, length + 2, stdout);
    std::fflush(stdout);
    std::memset(request_, 0, sizeof(request_));
    framer_.erase_line();
}
void SerialConsole::main()
{
    int64_t last_input = 0;
    bool paused = false;
    while (!should_exit()) {
        if (debug_probe::snapshot().mode == debug_probe::Mode::usb) {
            if (!paused) {
                const bool partial = framer_.partial();
                framer_.reset();
                if (partial) framer_.abandon(); // Quarantine its suffix across a PHY switch.
                last_input = 0;
            }
            paused = true;
        } else {
            paused = false;
            if (last_input && framer_.partial() && esp_timer_get_time() - last_input > 30'000'000) {
                framer_.abandon(); last_input = 0;
            }
            // IDF's default USB Serial/JTAG VFS reads poll hardware. Do not set
            // O_NONBLOCK without its interrupt driver (the ring buffer is absent).
            for (size_t bytes = 0; bytes < 128 && !should_exit(); ++bytes) {
                char input;
                if (read(STDIN_FILENO, &input, 1) <= 0) break;
                last_input = esp_timer_get_time();
                handle_line(framer_.push(input));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
} // namespace connectivity
