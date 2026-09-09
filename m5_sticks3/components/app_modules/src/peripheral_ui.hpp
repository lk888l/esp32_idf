#pragma once
#include <cstdint>
#include <memory>
#include "lvgl.h"

namespace app_modules {
// Called with the LVGL lock. Commands are asynchronous; no driver/NVS IO here.
class PeripheralUi final {
public:
    enum class Kind : uint8_t { speaker, microphone, infrared };
    PeripheralUi();
    ~PeripheralUi();
    PeripheralUi(const PeripheralUi&) = delete;
    PeripheralUi& operator=(const PeripheralUi&) = delete;
    lv_obj_t* open(Kind kind);
    // Load another screen first. Closing cancels capture, audio and infrared.
    void close();
    void next();
    void press();
    bool select(bool long_press);
    void update();
private:
    struct Preferences {
        uint32_t tone_hz = 1000, tone_ms = 2000, recording_ms = 5000;
        uint16_t ir_address = 0;
        uint8_t ir_command = 0, ir_repeats = 0, ir_slot = 0;
        bool ir_extended = false;
    } preferences_{};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace app_modules
