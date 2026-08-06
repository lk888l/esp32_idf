#pragma once

#include "wireless_esp32/settings_store.hpp"

#include "esp_event.h"
#include "esp_netif.h"

#include <atomic>
#include <cstdint>

namespace wireless_esp32 {

enum class WifiState : uint8_t { stopped = 0, ap_only = 1, connecting = 2, connected = 3 };

struct WifiManagerConfig {
    uint8_t ap_channel = 6;
    uint8_t max_ap_clients = 4;
};

class WifiManager {
public:
    explicit WifiManager(WifiManagerConfig config) : config_(config) {}
    ~WifiManager();

    bool initialize(const WirelessSettings& settings);
    bool apply_station(const WirelessSettings& settings);
    void process();
    bool deinitialize();

    [[nodiscard]] WifiState state() const { return state_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t station_ipv4() const
    {
        return station_ipv4_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint8_t ap_client_count() const
    {
        return ap_client_count_.load(std::memory_order_relaxed);
    }

private:
    static void event_handler(void* context,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void* event_data);
    void on_event(esp_event_base_t event_base, int32_t event_id, void* event_data);
    bool configure_ap(const WirelessSettings& settings);
    bool configure_station(const WirelessSettings& settings);

    WifiManagerConfig config_;
    esp_netif_t* ap_netif_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    std::atomic<WifiState> state_{WifiState::stopped};
    std::atomic<uint32_t> station_ipv4_{0};
    std::atomic<uint8_t> ap_client_count_{0};
    std::atomic<bool> reconnect_pending_{false};
    std::atomic<uint32_t> reconnect_at_ms_{0};
    std::atomic<uint8_t> retry_count_{0};
    bool wifi_initialized_ = false;
    bool wifi_started_ = false;
    bool station_configured_ = false;
};

} // namespace wireless_esp32
