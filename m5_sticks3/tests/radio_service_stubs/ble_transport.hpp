#pragma once
#include "platform.hpp"
#include "ble_diagnostics.hpp"
namespace fake { inline connectivity::BleDiagnostics ble_diag{}; }
namespace connectivity {
class BleTransport {
public:
    int start(const char*, RequestHandler handler, void* context)
    { fake::ble = handler; fake::context = context; fake::ble_diag = {}; fake::ble_diag.enabled = true; return ESP_OK; }
    int stop() { fake::ble_diag = {}; return ESP_OK; }
    void process() {}
    int request_scan() { fake::ble_diag.scanning = true; return ESP_OK; }
    int connect_peer(const char* address, uint8_t type, bool = false)
    {
        if (!fake::ble_diag.enabled) return ESP_ERR_INVALID_STATE;
        std::strcpy(fake::ble_diag.peer_address, address); fake::ble_diag.peer_address_type = type;
        fake::ble_diag.connecting = true; fake::ble_diag.peer_connected = false; return ESP_OK;
    }
    int disconnect_peer() { fake::ble_diag.connecting = fake::ble_diag.peer_connected = false; return ESP_OK; }
    int set_enabled(bool enabled) { fake::ble_diag.enabled = enabled; if (!enabled) disconnect_peer(); return ESP_OK; }
    int unpair(const char*, uint8_t, bool) { return ESP_OK; }
    BleSnapshot snapshot() { return {}; }
    BleDiagnostics diagnostics() { return fake::ble_diag; }
};
}
