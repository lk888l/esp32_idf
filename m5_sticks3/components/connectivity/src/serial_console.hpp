#pragma once
#include "app_task.hpp"
#include "connectivity_types.hpp"
#include "local_console_protocol.hpp"
#include "esp_err.h"

namespace connectivity {
// Dedicated bounded IO task: a disconnected/slow terminal cannot stall UI.
// The service owns the callback until stop_console has joined this task.
class SerialConsole final : private AppTask {
public:
    SerialConsole() : AppTask("radio_console", 10240, 2) {}
    esp_err_t start_console(RequestHandler handler, void* context);
    esp_err_t stop_console();
private:
    void main() override;
    void handle_line(console::LineFramer::Result result);
    RequestHandler handler_ = nullptr;
    void* context_ = nullptr;
    console::LineFramer framer_{};
    uint32_t sequence_ = 0;
    char request_[kMaxRequestBytes + 1]{};
    char output_[kMaxResponseBytes + 3]{};
};
} // namespace connectivity
