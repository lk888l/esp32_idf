#pragma once
#include <sys/time.h>
#include "platform.hpp"
#include "sdkconfig.h"
using esp_sntp_time_cb_t = void (*)(timeval*);
struct esp_sntp_config_t {
    bool smooth_sync = false, server_from_dhcp = false, wait_for_sync = true, start = true;
    esp_sntp_time_cb_t sync_cb = nullptr;
    size_t num_of_servers = 0;
    const char* servers[CONFIG_LWIP_SNTP_MAX_SERVERS]{};
};
namespace fake {
inline esp_sntp_config_t sntp_config{};
inline unsigned sntp_inits = 0, sntp_starts = 0, sntp_stops = 0;
inline int sntp_init_error = ESP_OK, sntp_start_error = ESP_OK;
inline bool sntp_active = false;
inline timeval* sntp_immediate_response = nullptr;
}
inline esp_err_t esp_netif_sntp_init(const esp_sntp_config_t* config)
{
    ++fake::sntp_inits;
    fake::sntp_config = *config;
    fake::sntp_active = fake::sntp_init_error == ESP_OK;
    return fake::sntp_init_error;
}
inline esp_err_t esp_netif_sntp_start()
{
    ++fake::sntp_starts;
    if (fake::sntp_start_error == ESP_OK && fake::sntp_immediate_response)
        fake::sntp_config.sync_cb(fake::sntp_immediate_response);
    return fake::sntp_start_error;
}
inline void esp_netif_sntp_deinit() { ++fake::sntp_stops; fake::sntp_active = false; }
