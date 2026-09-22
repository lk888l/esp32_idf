#pragma once
#include "platform.hpp"
#include "connectivity_policy.hpp"
#include "wifi_diagnostics.hpp"
namespace fake {
inline connectivity::WifiSnapshot wifi{};
inline connectivity::WifiDiagnostics wifi_diag{};
inline connectivity::WifiCredentials credentials{};
inline int configure_error = 0;
inline bool association_matches = true;
}
namespace connectivity {
class WifiTransport {
public:
    int start(RequestHandler handler, void* context, const char*, const char*, const WifiCredentials& initial)
    {
        fake::http = handler; fake::context = context; fake::wifi = {}; fake::wifi_diag = {};
        fake::wifi_diag.enabled = true; return configure(initial);
    }
    int stop() { fake::wifi = {}; fake::wifi_diag = {}; return ESP_OK; }
    void process() {}
    int configuration_ready() { return fake::wifi_diag.scanning ? ESP_ERR_INVALID_STATE : ESP_OK; }
    int configure(const WifiCredentials& value)
    {
        if (fake::configure_error) return fake::configure_error;
        fake::credentials = value; std::strcpy(fake::wifi.ssid, value.ssid);
        fake::wifi.state = value.ssid[0] ? WifiState::connecting : WifiState::access_point;
        fake::wifi.address[0] = 0; return ESP_OK;
    }
    int request_scan() { fake::wifi_diag.scanning = true; return ESP_OK; }
    bool connection_matches(const WifiCredentials&) { return fake::association_matches; }
    int set_enabled(bool enabled) { fake::wifi_diag.enabled = enabled; if (!enabled) fake::wifi.state = WifiState::stopped; return ESP_OK; }
    int disconnect_station() { fake::wifi.state = WifiState::access_point; fake::wifi.address[0] = 0; return ESP_OK; }
    WifiSnapshot snapshot() { return fake::wifi; }
    WifiDiagnostics diagnostics() { return fake::wifi_diag; }
};
}
