#pragma once

#include <cstdint>
#include "lvgl.h"

namespace app_modules {
class SleepUi {
public:
    lv_obj_t* open();
    void close(); // Caller loads another screen first; closing cancels a countdown.
    bool select(bool long_press);
    bool back(); // Cancel the countdown first, otherwise return to the menu.
    void update();
#ifdef M5_STICKS3_DAP_SMOKE_TEST
    void start_for_test(uint32_t timer_seconds);
#endif
private:
    void arm(uint32_t timer_seconds);
    lv_obj_t *screen_ = nullptr, *heading_ = nullptr, *mode_ = nullptr, *wake_ = nullptr,
             *status_ = nullptr, *time_ = nullptr, *controls_ = nullptr, *hint_ = nullptr;
    unsigned selection_ = 0;
    uint32_t countdown_start_ = 0;
    uint32_t timer_seconds_ = 0;
    bool countdown_ = false;
    bool submitted_ = false;
    bool refused_ = false;
};
} // namespace app_modules
