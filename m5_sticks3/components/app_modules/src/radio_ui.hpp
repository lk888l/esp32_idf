#pragma once

#include <memory>
#include <cstdint>
#include "lvgl.h"

namespace app_modules {

// All methods are called with the existing LVGL port lock held. Radio actions
// are value requests to the main-loop command queue, never driver calls.
class RadioUi final {
public:
    enum class Kind : uint8_t { wifi, bluetooth };
    RadioUi();
    ~RadioUi();
    RadioUi(const RadioUi&) = delete;
    RadioUi& operator=(const RadioUi&) = delete;

    lv_obj_t* open(Kind kind);
    // The caller must load another screen before releasing this one.
    void close();
    void next();
    void press();
    bool select(bool long_press); // true asks the controller to return to menu
    void update();
#ifdef M5_STICKS3_RADIO_SMOKE_TEST
    void smoke_step(uint8_t step);
    void capture(const char* name);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace app_modules
