#pragma once
#include "debug_probe.hpp"
#include "lvgl.h"
namespace app_modules {
class DebugUi {
public:
    lv_obj_t* open(debug_probe::Mode mode);
    void close(); // Caller loads another screen first.
    void next();
    bool select(bool long_press);
    void update();
private:
    lv_obj_t *screen_ = nullptr, *title_ = nullptr, *status_ = nullptr, *clock_ = nullptr,
             *details_ = nullptr, *action_ = nullptr, *hint_ = nullptr;
    debug_probe::Mode mode_ = debug_probe::Mode::off;
    unsigned selected_ = 0, preset_ = 3;
    bool wiring_ = false, paused_ = false;
    uint32_t refresh_ = 0;
    esp_err_t control_error_ = ESP_OK;
};
}
