#pragma once
#include "platform.hpp"
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT
};
namespace fake {
inline esp_reset_reason_t reset_reason = ESP_RST_POWERON;
}
inline esp_reset_reason_t esp_reset_reason() { return fake::reset_reason; }
