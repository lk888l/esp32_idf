#pragma once

#include <cstddef>
#include <cstdint>

namespace connectivity {

// Called on an IDF transport task. Handlers must be bounded and nonblocking.
// The handler always produces one NUL-terminated JSON response.
using RequestHandler = void (*)(void* context, const char* request, size_t length,
                                char* response, size_t capacity);

enum class WifiState : uint8_t { stopped, access_point, connecting, connected, retry_wait, failed };

struct WifiSnapshot {
    WifiState state = WifiState::stopped;
    bool ap_active = false;
    char ssid[33]{};
    char address[16]{};
    char ap_ssid[33]{};
    char ap_address[16]{};
    int8_t rssi = 0;
    uint32_t reconnects = 0;
    uint16_t disconnect_reason = 0;
};

struct BleSnapshot {
    bool enabled = false;
    bool advertising = false;
    bool connected = false;
    bool subscribed = false;
    bool encrypted = false;
    bool bonded = false;
    uint16_t mtu = 23;
    uint32_t received = 0;
    uint32_t dropped = 0;
};

} // namespace connectivity
