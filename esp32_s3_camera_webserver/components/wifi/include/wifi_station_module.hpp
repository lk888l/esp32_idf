#pragma once

#include "app_module.hpp"
#include "logger.hpp"

#include <string_view>

#include "esp_event_base.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace wifi {

class WiFiStationModule final : public AppModule {
public:
    bool initialize() override;
    bool deinitialize() override;
    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "wifi_station"; }

private:
    static void eventHandler(void* arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void* event_data);

    void handleEvent(esp_event_base_t event_base, int32_t event_id, void* event_data);
    bool initializeNvs();

    ium::Logger log_{"wifi"};
    EventGroupHandle_t event_group_ = nullptr;
    esp_netif_t* netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    int retry_count_ = 0;
    bool initialized_ = false;
};

} // namespace wifi
