#include "clock_service.hpp"

#include <cstdlib>
#include <ctime>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace clock_service {
namespace {
constexpr char kTag[] = "clock";
constexpr int64_t kEarliestEpoch = 1704067200; // 2024-01-01 UTC
constexpr int64_t kResponseDeadlineUs = 60'000'000;
constexpr int64_t kStartRetryUs = 60'000'000;
constexpr int64_t kManualCooldownUs = 10'000'000;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot shared{};
bool ready = false;
bool sync_requested = false;
int64_t last_sync_us = -1;
int64_t last_request_us = -1;

void time_received(timeval* value)
{
    if (!value || value->tv_sec < kEarliestEpoch) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (ready) {
        shared.synced = true;
        shared.last_sync_epoch = value->tv_sec;
        ++shared.sync_count;
        shared.error = ESP_OK;
        shared.state = shared.online ? State::synced : State::waiting_wifi;
        last_sync_us = now;
    }
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGI(kTag, "SNTP synchronized: epoch=%lld", static_cast<long long>(value->tv_sec));
}
} // namespace

esp_err_t Service::initialize()
{
    if (initialized_) return ESP_OK;
    portENTER_CRITICAL(&state_lock);
    const bool occupied = ready;
    portEXIT_CRITICAL(&state_lock);
    if (occupied) return ESP_ERR_INVALID_STATE;
    if (setenv("TZ", CONFIG_M5_CLOCK_TIMEZONE, 1) != 0) return ESP_ERR_NO_MEM;
    tzset();
    portENTER_CRITICAL(&state_lock);
    shared = {};
    shared.state = State::waiting_wifi;
    last_sync_us = last_request_us = -1;
    sync_requested = false;
    ready = true;
    portEXIT_CRITICAL(&state_lock);
    initialized_ = true;
    ESP_LOGI(kTag, "ready: %s; NTP %s / %s; interval=%lu s",
             timezone_label(), CONFIG_M5_CLOCK_NTP_SERVER, CONFIG_M5_CLOCK_NTP_SERVER_BACKUP,
             static_cast<unsigned long>(sync_interval_s()));
    return ESP_OK;
}

void Service::process(bool station_connected)
{
    if (!initialized_) return;
    const int64_t now = esp_timer_get_time();
    bool requested = false;
    portENTER_CRITICAL(&state_lock);
    shared.online = station_connected;
    requested = sync_requested;
    sync_requested = false;
    if (!station_connected) {
        shared.state = State::waiting_wifi;
        shared.error = ESP_OK;
    }
    portEXIT_CRITICAL(&state_lock);

    if (!station_connected) {
        // Keep system time ticking; release SNTP and its retry timers offline.
        if (sntp_initialized_) esp_netif_sntp_deinit();
        sntp_initialized_ = connected_ = false;
        attempt_us_ = -1;
        retry_at_us_ = 0;
        return;
    }

    const bool connect_edge = !connected_;
    connected_ = true;
    if (connect_edge || requested || (!sntp_initialized_ && now >= retry_at_us_)) {
        esp_err_t result = ESP_OK;
        if (!sntp_initialized_) {
            // Names are configuration literals with lifetime beyond this call.
            esp_sntp_config_t config{};
            config.start = false;
            config.wait_for_sync = false;
            config.sync_cb = time_received;
            config.num_of_servers = 2;
            config.servers[0] = CONFIG_M5_CLOCK_NTP_SERVER;
            config.servers[1] = CONFIG_M5_CLOCK_NTP_SERVER_BACKUP;
            static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 2, "Clock needs two SNTP server slots");
            result = esp_netif_sntp_init(&config);
            sntp_initialized_ = result == ESP_OK;
        }
        // Publish before start: a response can arrive before start returns.
        portENTER_CRITICAL(&state_lock);
        shared.state = State::syncing;
        shared.error = ESP_OK;
        last_request_us = now;
        portEXIT_CRITICAL(&state_lock);
        attempt_us_ = now;
        if (result == ESP_OK) result = esp_netif_sntp_start();
        if (result != ESP_OK) {
            if (sntp_initialized_) esp_netif_sntp_deinit();
            sntp_initialized_ = false;
            retry_at_us_ = now + kStartRetryUs;
            portENTER_CRITICAL(&state_lock);
            shared.state = State::retrying;
            shared.error = result;
            portEXIT_CRITICAL(&state_lock);
            ESP_LOGW(kTag, "SNTP start failed: %s", esp_err_to_name(result));
        }
    }

    portENTER_CRITICAL(&state_lock);
    const bool waiting = last_sync_us < attempt_us_ && now - attempt_us_ >= kResponseDeadlineUs;
    const bool stale = last_sync_us >= 0 && now - last_sync_us >=
        static_cast<int64_t>(sync_interval_s()) * 1'000'000 + kResponseDeadlineUs;
    if (sntp_initialized_ && (waiting || stale)) {
        shared.state = State::retrying;
        shared.error = ESP_ERR_TIMEOUT;
        // Leave lwIP running so it can rotate servers and apply its backoff.
    }
    portEXIT_CRITICAL(&state_lock);
}

void Service::deinitialize()
{
    if (!initialized_) return;
    portENTER_CRITICAL(&state_lock);
    ready = false;
    sync_requested = false;
    portEXIT_CRITICAL(&state_lock);
    // The synchronous TCP/IP barrier also drains any in-flight sync callback.
    if (sntp_initialized_) esp_netif_sntp_deinit();
    sntp_initialized_ = initialized_ = connected_ = false;
    attempt_us_ = -1;
    retry_at_us_ = 0;
    portENTER_CRITICAL(&state_lock);
    shared = {};
    last_sync_us = last_request_us = -1;
    portEXIT_CRITICAL(&state_lock);
}

esp_err_t request_sync()
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    esp_err_t result = ESP_OK;
    if (!ready || !shared.online) result = ESP_ERR_INVALID_STATE;
    else if (sync_requested || shared.state == State::syncing) {} // Coalesce.
    else if (last_request_us >= 0 && now - last_request_us < kManualCooldownUs)
        result = ESP_ERR_TIMEOUT;
    else {
        sync_requested = true;
        shared.state = State::syncing;
        shared.error = ESP_OK;
    }
    portEXIT_CRITICAL(&state_lock);
    return result;
}

Snapshot snapshot()
{
    Snapshot result;
    int64_t synchronized_at;
    portENTER_CRITICAL(&state_lock);
    result = shared;
    synchronized_at = last_sync_us;
    portEXIT_CRITICAL(&state_lock);
    if (synchronized_at >= 0) result.sync_age_s =
        (esp_timer_get_time() - synchronized_at) / 1'000'000;
    const time_t now = std::time(nullptr);
    result.valid = result.synced && now >= kEarliestEpoch;
    if (result.valid) {
        tm local{};
        if (localtime_r(&now, &local)) {
            result.epoch = now;
            std::strftime(result.time, sizeof(result.time), "%H:%M:%S", &local);
            std::strftime(result.date, sizeof(result.date), "%Y/%m/%d", &local);
            std::strftime(result.weekday, sizeof(result.weekday), "%a", &local);
        } else result.valid = false;
    }
    return result;
}

const char* state_name(State state)
{
    switch (state) {
    case State::stopped: return "off";
    case State::waiting_wifi: return "waiting_wifi";
    case State::syncing: return "syncing";
    case State::synced: return "synced";
    case State::retrying: return "retrying";
    }
    return "unknown";
}
const char* timezone() { return CONFIG_M5_CLOCK_TIMEZONE; }
const char* timezone_label() { return CONFIG_M5_CLOCK_TIMEZONE_LABEL; }
uint32_t sync_interval_s() { return CONFIG_LWIP_SNTP_UPDATE_DELAY / 1000; }
} // namespace clock_service
