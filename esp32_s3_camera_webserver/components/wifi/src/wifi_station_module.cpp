#include "wifi_station_module.hpp"

#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailBit = BIT1;

} // namespace

namespace wifi {

bool WiFiStationModule::initializeNvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        log_.error("nvs_flash_init failed: 0x{:x}", static_cast<int>(ret));
        return false;
    }

    return true;
}

bool WiFiStationModule::initialize()
{
    if (initialized_) {
        return true;
    }

    if (std::strcmp(CONFIG_CAMERA_WEB_WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
        log_.warn("Wi-Fi SSID is still the default placeholder");
    }

    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        log_.error("failed to create Wi-Fi event group");
        return false;
    }

    if (!initializeNvs()) {
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t event_loop_ret = esp_event_loop_create_default();
    if (event_loop_ret != ESP_OK && event_loop_ret != ESP_ERR_INVALID_STATE) {
        log_.error("esp_event_loop_create_default failed: 0x{:x}", static_cast<int>(event_loop_ret));
        return false;
    }

    netif_ = esp_netif_create_default_wifi_sta();
    if (netif_ == nullptr) {
        log_.error("failed to create default Wi-Fi station netif");
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &WiFiStationModule::eventHandler,
        this,
        &wifi_handler_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &WiFiStationModule::eventHandler,
        this,
        &ip_handler_));

    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 CONFIG_CAMERA_WEB_WIFI_SSID,
                 sizeof(wifi_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 CONFIG_CAMERA_WEB_WIFI_PASSWORD,
                 sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    log_.info("connecting to SSID '{}'", CONFIG_CAMERA_WEB_WIFI_SSID);
    const EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        kWifiConnectedBit | kWifiFailBit,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_CAMERA_WEB_WIFI_CONNECT_TIMEOUT_MS));

    if ((bits & kWifiConnectedBit) == 0) {
        log_.error("failed to connect to Wi-Fi");
        return false;
    }

    initialized_ = true;
    return true;
}

bool WiFiStationModule::deinitialize()
{
    if (!initialized_) {
        return true;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (wifi_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_);
        wifi_handler_ = nullptr;
    }
    if (ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
        ip_handler_ = nullptr;
    }
    if (netif_ != nullptr) {
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }

    initialized_ = false;
    return true;
}

void WiFiStationModule::eventHandler(void* arg,
                                     esp_event_base_t event_base,
                                     int32_t event_id,
                                     void* event_data)
{
    auto* self = static_cast<WiFiStationModule*>(arg);
    if (self != nullptr) {
        self->handleEvent(event_base, event_id, event_data);
    }
}

void WiFiStationModule::handleEvent(esp_event_base_t event_base,
                                    int32_t event_id,
                                    void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count_ < CONFIG_CAMERA_WEB_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            ++retry_count_;
            log_.warn("retrying Wi-Fi connection ({}/{})",
                      retry_count_,
                      CONFIG_CAMERA_WEB_WIFI_MAXIMUM_RETRY);
        } else if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kWifiFailBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        retry_count_ = 0;
        char ip_addr[16]{};
        std::snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        log_.info("connected, ip={}", ip_addr);
        if (event_group_ != nullptr) {
            xEventGroupSetBits(event_group_, kWifiConnectedBit);
        }
    }
}

} // namespace wifi
