#pragma once

#include <array>
#include <cstdint>
#include "lvgl.h"

namespace app_modules {
class PcMonitorUi {
public:
    lv_obj_t* open();
    void close(); // Caller loads another screen first.
    bool select(bool long_press);
    void update();
private:
    lv_obj_t *screen_ = nullptr, *status_ = nullptr, *host_ = nullptr, *address_ = nullptr;
    std::array<lv_obj_t*, 3> captions_{}, values_{}, bars_{};
    uint32_t refreshed_ = 0;
    bool details_ = false;
};
} // namespace app_modules
