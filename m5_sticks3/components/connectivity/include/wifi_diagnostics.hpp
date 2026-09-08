#pragma once

#include <cstddef>
#include <cstdint>

namespace connectivity {

constexpr size_t kMaxWifiNetworks = 16;

// auth is the native wifi_auth_mode_t value. A discovered network is not
// necessarily connectable with the personal-password authentication UI.
struct WifiNetwork {
    char ssid[33]{};
    char bssid[18]{};
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint8_t auth = 0;
};

struct WifiDiagnostics {
    bool enabled = false;
    bool station_enabled = false;
    bool scanning = false;
    uint8_t channel = 0;
    uint8_t ap_clients = 0;
    char bssid[18]{};
    uint16_t total_found = 0;
    uint8_t count = 0;
    // Changes after every completed/failed/cancelled scan. The previous
    // successful list is preserved while scanning and on error.
    uint32_t generation = 0;
    int32_t scan_error = 0;
    WifiNetwork networks[kMaxWifiNetworks]{};
};

} // namespace connectivity
