#pragma once

#include <cstdint>
#include "esp_err.h"

namespace clock_service {

enum class State : uint8_t { stopped, waiting_wifi, syncing, synced, retrying };

struct Snapshot {
    State state = State::stopped;
    bool online = false;
    bool valid = false;
    bool synced = false; // A successful SNTP response in this boot.
    int64_t epoch = 0;
    int64_t last_sync_epoch = 0;
    int64_t sync_age_s = -1;
    uint32_t sync_count = 0;
    esp_err_t error = ESP_OK;
    char time[9] = "--:--:--";
    char date[11] = "----/--/--";
    char weekday[4] = "---";
};

// One owner, called from the application task after esp_netif_init().
// SNTP starts only with a station IP. All network waits happen in lwIP.
class Service {
public:
    esp_err_t initialize();
    void process(bool station_connected);
    void deinitialize();
    Service() = default;
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
private:
    bool initialized_ = false;
    bool sntp_initialized_ = false;
    bool connected_ = false;
    int64_t attempt_us_ = -1;
    int64_t retry_at_us_ = 0;
};

// Safe from the UI and transport tasks; requests coalesce until processed.
// ESP_OK means requested, not synchronized. Poll snapshot() for success.
esp_err_t request_sync();
Snapshot snapshot();
const char* state_name(State state);
const char* timezone();
const char* timezone_label();
uint32_t sync_interval_s();

} // namespace clock_service
