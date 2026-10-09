#pragma once
#include "clock_service.hpp"
#include "lvgl.h"

namespace app_modules {
class ClockUi {
public:
    lv_obj_t* open();
    void close(); // Caller loads another screen first.
    bool select(bool long_press);
    void update();
private:
    lv_obj_t *screen_ = nullptr, *hours_ = nullptr, *seconds_ = nullptr,
             *progress_ = nullptr, *date_ = nullptr, *status_ = nullptr, *age_ = nullptr;
    uint32_t refresh_ = 0;
    uint32_t message_until_ = 0;
    esp_err_t request_error_ = ESP_OK;
};
} // namespace app_modules
