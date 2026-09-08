#pragma once
#include "connectivity_types.hpp"
#include "connectivity_policy.hpp"
#include "wifi_diagnostics.hpp"
#include "ble_diagnostics.hpp"
#include "esp_err.h"
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
enum class ControlAction : uint8_t {
    wifi_scan, wifi_connect, wifi_reconnect, wifi_disconnect,
    wifi_enable, wifi_disable, wifi_clear, ble_scan, ble_connect,
    ble_disconnect, ble_enable, ble_disable
};
struct ControlRequest {
    ControlAction action = ControlAction::wifi_scan;
    WifiCredentials wifi{};
    char address[18]{};
    uint8_t address_type = 0;
};
struct TrafficSnapshot {
    uint32_t http_requests = 0;
    uint32_t ble_requests = 0;
    uint32_t rx_bytes = 0;
    uint32_t tx_bytes = 0;
    char last_transport[5]{};
    char last_operation[24]{};
    int32_t last_request_id = 0;
    bool last_ok = false;
    char echo[65]{};
};
// Physical UI requests join the same bounded queue as authenticated API commands.
// ESP_OK means queued; completion/error is published in snapshot().
esp_err_t request_control(const ControlRequest& request);
// POD copies under short critical sections; safe for LVGL and transports.
Snapshot snapshot();
WifiDiagnostics wifi_diagnostics();
BleDiagnostics ble_diagnostics();
TrafficSnapshot traffic_snapshot();
// Physical display only. Never expose this helper through a radio request.
bool copy_local_ap_password(char* output, size_t capacity);
const char* wifi_state_name(WifiState state);
}
