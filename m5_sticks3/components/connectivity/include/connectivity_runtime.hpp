#pragma once
#include "connectivity_types.hpp"
namespace connectivity {
struct Snapshot {
    WifiSnapshot wifi{};
    BleSnapshot ble{};
    bool ready = false;
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    uint32_t completed = 0;
    uint32_t last_id = 0;
    int32_t last_error = 0;
};
// POD copy protected by a short critical section; safe for LVGL and transports.
Snapshot snapshot();
const char* wifi_state_name(WifiState state);
}
