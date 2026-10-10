#pragma once

#include <cstdint>
#include "esp_err.h"

namespace app { class AppManager; }

namespace sleep_service {
// The UI only posts a request. The main task owns module teardown and sleep.
// Zero means button wake only; a nonzero timer adds an alternative wake source.
bool request_sleep(uint32_t timer_seconds);
bool take_request(uint32_t& timer_seconds);
esp_err_t last_error();

// Release held peripheral pads before any normal drivers are initialized.
void restore_after_wake();
// Returns only on failure. Never power-gates the ESP32's L2 supply.
esp_err_t enter_deep_sleep(app::AppManager& manager, uint32_t timer_seconds);
} // namespace sleep_service
