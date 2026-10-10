#include "clock_service.hpp"

#include <cstdlib>
#include <ctime>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace clock_service {
namespace {
constexpr char kTag[] = "clock";
constexpr int64_t kEarliestEpoch = 1704067200; // 2024-01-01 UTC
constexpr int64_t kLatestEpoch = 4102444800; // 2100-01-01 UTC
constexpr int64_t kResponseDeadlineUs = 60'000'000;
constexpr int64_t kStartRetryUs = 60'000'000;
constexpr int64_t kManualCooldownUs = 10'000'000;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot shared{};
bool ready = false;
bool sync_requested = false;
int64_t last_sync_us = -1;
int64_t last_request_us = -1;
int64_t sync_age_base_s = -1;
int64_t sync_age_base_us = -1;

struct RetainedClock {
    uint32_t magic = 0;
    uint32_t sync_count = 0;
    int64_t last_sync_epoch = 0;
    int64_t sleep_epoch = 0;
    int64_t sync_age_s = -1;
    uint32_t checksum = 0;
};
constexpr uint32_t kRetainedMagic = 0x434C4B32; // CLK2, metadata version 2.
RTC_DATA_ATTR RetainedClock retained{};

bool sane_epoch(int64_t epoch)
{
    return epoch >= kEarliestEpoch && epoch < kLatestEpoch;
}

uint32_t checksum(const RetainedClock& value)
{
    // Hash fields explicitly: compiler padding is not part of the RTC format.
    uint32_t hash = 2166136261U;
    const uint64_t fields[] = {uint64_t(value.magic), uint64_t(value.sync_count),
                               uint64_t(value.last_sync_epoch), uint64_t(value.sleep_epoch),
                               uint64_t(value.sync_age_s)};
    for (const uint64_t field : fields) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<uint8_t>(field >> shift);
            hash *= 16777619U;
        }
    }
    return hash;
}

// Called while state_lock is held. The age baseline can include previous boots
// and sleep time; esp_timer_get_time() restarts from zero after deep sleep.
int64_t synchronization_age_s(int64_t now)
{
    if (sync_age_base_s < 0 || sync_age_base_us < 0) return -1;
    return sync_age_base_s + (now - sync_age_base_us) / 1'000'000;
}

void time_received(timeval* value)
{
    if (!value || !sane_epoch(value->tv_sec)) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (ready) {
        shared.synced = true;
        shared.last_sync_epoch = value->tv_sec;
        ++shared.sync_count;
        shared.error = ESP_OK;
        shared.state = shared.online ? State::synced : State::waiting_wifi;
        last_sync_us = now;
        sync_age_base_s = 0;
        sync_age_base_us = now;
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
    const int64_t epoch = std::time(nullptr);
    const int64_t now = esp_timer_get_time();
    const bool restore = esp_reset_reason() == ESP_RST_DEEPSLEEP &&
        retained.magic == kRetainedMagic && retained.checksum == checksum(retained) &&
        retained.sync_count > 0 && sane_epoch(epoch) && sane_epoch(retained.sleep_epoch) &&
        sane_epoch(retained.last_sync_epoch) && retained.last_sync_epoch <= retained.sleep_epoch &&
        epoch >= retained.sleep_epoch && retained.sync_age_s >= 0 &&
        retained.sync_age_s < kLatestEpoch - kEarliestEpoch;
    portENTER_CRITICAL(&state_lock);
    shared = {};
    shared.state = State::waiting_wifi;
    last_sync_us = last_request_us = -1;
    sync_age_base_s = sync_age_base_us = -1;
    if (restore) {
        shared.synced = true;
        shared.last_sync_epoch = retained.last_sync_epoch;
        shared.sync_count = retained.sync_count;
        sync_age_base_s = retained.sync_age_s + epoch - retained.sleep_epoch;
        sync_age_base_us = now;
    }
    // Consume the marker. Reinitializing a service in the same boot must not
    // reuse a sleep entry, and failed sleep preparation cannot survive a reset.
    retained = {};
    sync_requested = false;
    ready = true;
    portEXIT_CRITICAL(&state_lock);
    initialized_ = true;
    if (restore) ESP_LOGI(kTag, "RTC restored after deep sleep: epoch=%lld; sync age=%lld s",
                         static_cast<long long>(epoch), static_cast<long long>(sync_age_base_s));
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
    const bool stale = synchronization_age_s(now) >=
        static_cast<int64_t>(sync_interval_s()) + kResponseDeadlineUs / 1'000'000;
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
    sync_age_base_s = sync_age_base_us = -1;
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
    portENTER_CRITICAL(&state_lock);
    result = shared;
    result.sync_age_s = synchronization_age_s(esp_timer_get_time());
    portEXIT_CRITICAL(&state_lock);
    const time_t now = std::time(nullptr);
    result.valid = result.synced && sane_epoch(now);
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

void prepare_for_sleep()
{
    const auto value = snapshot();
    RetainedClock next{};
    if (value.valid && value.sync_count > 0 && value.sync_age_s >= 0 &&
        value.last_sync_epoch <= value.epoch) {
        next.magic = kRetainedMagic;
        next.sync_count = value.sync_count;
        next.last_sync_epoch = value.last_sync_epoch;
        next.sleep_epoch = value.epoch;
        next.sync_age_s = value.sync_age_s;
        next.checksum = checksum(next);
    }
    portENTER_CRITICAL(&state_lock);
    retained = next;
    portEXIT_CRITICAL(&state_lock);
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
