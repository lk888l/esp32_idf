#include "wireless_esp32/wifi_manager.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cstring>

namespace wireless_esp32 {
namespace {
constexpr char kTag[] = "wifi_gateway";

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000U);
}

bool ok_or_invalid_state(esp_err_t result)
{
    return result == ESP_OK || result == ESP_ERR_INVALID_STATE;
}

} // namespace

WifiManager::~WifiManager()
{
    (void)deinitialize();
}

bool WifiManager::initialize(const WirelessSettings& settings)
{
    if (wifi_initialized_) {
        return true;
    }
    if (!ok_or_invalid_state(esp_netif_init()) ||
        !ok_or_invalid_state(esp_event_loop_create_default())) {
        return false;
    }

    ap_netif_ = esp_netif_create_default_wifi_ap();
    sta_netif_ = esp_netif_create_default_wifi_sta();
    if (ap_netif_ == nullptr || sta_netif_ == nullptr ||
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, this, &wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, this, &ip_handler_) != ESP_OK) {
        (void)deinitialize();
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_config) != ESP_OK) {
        (void)deinitialize();
        return false;
    }
    wifi_initialized_ = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(settings.station_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP) != ESP_OK ||
        !configure_ap(settings) ||
        (settings.station_configured && !configure_station(settings)) ||
        esp_wifi_start() != ESP_OK) {
        (void)deinitialize();
        return false;
    }
    wifi_started_ = true;
    station_configured_ = settings.station_configured;
    state_.store(station_configured_ ? WifiState::connecting : WifiState::ap_only,
                 std::memory_order_relaxed);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(kTag,
             "SoftAP ready: SSID=%s channel=%u, station=%s",
             settings.ap_ssid.data(),
             config_.ap_channel,
             station_configured_ ? "configured" : "not configured");
    return true;
}

bool WifiManager::configure_ap(const WirelessSettings& settings)
{
    wifi_config_t config{};
    std::memcpy(config.ap.ssid, settings.ap_ssid.data(), settings.ap_ssid.size());
    config.ap.ssid_len = static_cast<uint8_t>(std::strlen(settings.ap_ssid.data()));
    std::memcpy(
        config.ap.password, settings.ap_password.data(), settings.ap_password.size());
    config.ap.channel = config_.ap_channel;
    config.ap.max_connection = config_.max_ap_clients;
    config.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;
    config.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK;
}

bool WifiManager::configure_station(const WirelessSettings& settings)
{
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, settings.sta_ssid.data(), settings.sta_ssid.size());
    std::memcpy(
        config.sta.password, settings.sta_password.data(), settings.sta_password.size());
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode =
        settings.sta_password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK;
}

bool WifiManager::apply_station(const WirelessSettings& settings)
{
    if (!wifi_initialized_ || !wifi_started_ || !settings.station_configured ||
        esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        !configure_station(settings)) {
        return false;
    }
    station_configured_ = true;
    retry_count_.store(0, std::memory_order_relaxed);
    station_ipv4_.store(0, std::memory_order_relaxed);
    state_.store(WifiState::connecting, std::memory_order_relaxed);
    (void)esp_wifi_disconnect();
    const esp_err_t result = esp_wifi_connect();
    return result == ESP_OK || result == ESP_ERR_WIFI_CONN;
}

void WifiManager::process()
{
    if (!reconnect_pending_.load(std::memory_order_acquire) || !station_configured_ ||
        static_cast<int32_t>(now_ms() - reconnect_at_ms_.load(std::memory_order_relaxed)) < 0) {
        return;
    }
    reconnect_pending_.store(false, std::memory_order_release);
    state_.store(WifiState::connecting, std::memory_order_relaxed);
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        reconnect_pending_.store(true, std::memory_order_release);
        reconnect_at_ms_.store(now_ms() + 5000U, std::memory_order_relaxed);
    }
}

bool WifiManager::deinitialize()
{
    reconnect_pending_.store(false, std::memory_order_release);
    bool success = true;
    if (wifi_started_) {
        success = ok_or_invalid_state(esp_wifi_stop()) && success;
        wifi_started_ = false;
    }
    if (wifi_initialized_) {
        success = esp_wifi_deinit() == ESP_OK && success;
        wifi_initialized_ = false;
    }
    if (ip_handler_ != nullptr) {
        success = esp_event_handler_instance_unregister(
                      IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_) == ESP_OK &&
                  success;
        ip_handler_ = nullptr;
    }
    if (wifi_handler_ != nullptr) {
        success = esp_event_handler_instance_unregister(
                      WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_) == ESP_OK &&
                  success;
        wifi_handler_ = nullptr;
    }
    if (sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    state_.store(WifiState::stopped, std::memory_order_relaxed);
    return success;
}

void WifiManager::event_handler(void* context,
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void* event_data)
{
    static_cast<WifiManager*>(context)->on_event(event_base, event_id, event_data);
}

void WifiManager::on_event(esp_event_base_t event_base,
                           int32_t event_id,
                           void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START &&
        station_configured_) {
        reconnect_at_ms_.store(now_ms(), std::memory_order_relaxed);
        reconnect_pending_.store(true, std::memory_order_release);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
               station_configured_) {
        station_ipv4_.store(0, std::memory_order_relaxed);
        const uint8_t retry =
            std::min<uint8_t>(retry_count_.fetch_add(1, std::memory_order_relaxed), 5);
        const uint32_t delay_ms = std::min<uint32_t>(1000U << retry, 30000U);
        reconnect_at_ms_.store(now_ms() + delay_ms, std::memory_order_relaxed);
        reconnect_pending_.store(true, std::memory_order_release);
        state_.store(WifiState::connecting, std::memory_order_relaxed);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        station_ipv4_.store(event->ip_info.ip.addr, std::memory_order_relaxed);
        retry_count_.store(0, std::memory_order_relaxed);
        reconnect_pending_.store(false, std::memory_order_release);
        state_.store(WifiState::connected, std::memory_order_relaxed);
        ESP_LOGI(kTag, "station acquired IPv4: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const uint8_t current = ap_client_count_.load(std::memory_order_relaxed);
        if (current < UINT8_MAX) {
            ap_client_count_.store(current + 1, std::memory_order_relaxed);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const uint8_t current = ap_client_count_.load(std::memory_order_relaxed);
        if (current > 0) {
            ap_client_count_.store(current - 1, std::memory_order_relaxed);
        }
    }
}

} // namespace wireless_esp32
