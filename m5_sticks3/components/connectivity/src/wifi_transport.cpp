#include "wifi_transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <strings.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

namespace connectivity {
#if CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
namespace {
constexpr char kTag[] = "WifiTransport";
constexpr int64_t kConnectTimeoutUs = 30'000'000;
constexpr int64_t kBodyTimeoutUs = 2'000'000;
constexpr int64_t kRssiIntervalUs = 5'000'000;
constexpr int64_t kScanTimeoutUs = 10'000'000;

class StateGuard final {
public:
    explicit StateGuard(portMUX_TYPE& lock) : lock_(lock) { portENTER_CRITICAL(&lock_); }
    ~StateGuard() { portEXIT_CRITICAL(&lock_); }
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;
private:
    portMUX_TYPE& lock_;
};

void format_bssid(char (&output)[18], const uint8_t* address)
{
    std::snprintf(output, sizeof(output), "%02X:%02X:%02X:%02X:%02X:%02X",
        address[0], address[1], address[2], address[3], address[4], address[5]);
}

bool credentials_valid_or_disabled(const WifiCredentials& credentials)
{
    return credentials.ssid[0] == '\0'
        ? credentials.password[0] == '\0'
        : valid_credentials(credentials);
}

wifi_config_t station_config(const WifiCredentials& credentials)
{
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, credentials.ssid, std::strlen(credentials.ssid));
    std::memcpy(config.sta.password, credentials.password, std::strlen(credentials.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // Open networks require an explicitly empty password; reject WEP/WPA1.
    config.sta.threshold.authmode = credentials.password[0] == '\0'
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.failure_retry_cnt = 0; // Retry policy belongs to process().
    return config;
}

esp_err_t send_json(httpd_req_t* request, const char* status,
                    const char* body, size_t length)
{
    esp_err_t result = httpd_resp_set_status(request, status);
    if (result == ESP_OK) result = httpd_resp_set_type(request, "application/json");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    if (result == ESP_OK) result = httpd_resp_send(request, body, length);
    return result;
}

void drain_rejected_body(httpd_req_t* request)
{
    // Returning ESP_FAIL bypasses IDF's normal body purge. Unread TCP data can
    // otherwise make close send an RST that hides our small error response.
    // Its built-in purge has no byte limit, so use a bounded drain instead.
    constexpr size_t kDrainLimit = 1024;
    const int socket = httpd_req_to_sockfd(request);
    if (socket < 0) return;
    const int64_t deadline = esp_timer_get_time() + kBodyTimeoutUs;
    size_t remaining_budget = kDrainLimit;
    char discard[128];
    while (remaining_budget != 0) {
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) break;
        timeval timeout{};
        timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(remaining_us / 1'000'000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining_us % 1'000'000);
        // The session always closes below, so its receive timeout need not be
        // restored. Each receive consumes only the remaining overall deadline.
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) break;
        const size_t count = remaining_budget < sizeof(discard) ? remaining_budget : sizeof(discard);
        // IDF clamps this to its internally tracked unread body length and
        // returns zero immediately when the body has already been consumed.
        const int received = httpd_req_recv(request, discard, count);
        if (received <= 0) break;
        remaining_budget -= static_cast<size_t>(received);
    }
}

esp_err_t reject_request(httpd_req_t* request, const char* status, const char* error,
                         bool drain_body = true)
{
    if (drain_body) drain_rejected_body(request);
    char response[128]{};
    const int length = std::snprintf(response, sizeof(response),
        "{\"v\":1,\"id\":0,\"ok\":false,\"error\":\"%s\"}", error);
    const esp_err_t result = length > 0 && static_cast<size_t>(length) < sizeof(response)
        ? send_json(request, status, response, static_cast<size_t>(length)) : ESP_FAIL;
    if (result != ESP_OK) ESP_LOGD(kTag, "HTTP error response: %s", esp_err_to_name(result));
    // Close after the bounded purge; never reuse a rejected request session.
    return ESP_FAIL;
}

bool json_content_type(httpd_req_t* request)
{
    char content_type[64]{};
    const size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (length == 0 || length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK) return false;
    constexpr size_t prefix_length = sizeof("application/json") - 1;
    if (strncasecmp(content_type, "application/json", prefix_length) != 0) return false;
    const char* suffix = content_type + prefix_length;
    while (*suffix == ' ' || *suffix == '\t') ++suffix;
    return *suffix == '\0' || *suffix == ';';
}
} // namespace

WifiTransport::~WifiTransport()
{
    const esp_err_t result = stop();
    if (result != ESP_OK) ESP_LOGE(kTag, "Shutdown incomplete: %s", esp_err_to_name(result));
}

esp_err_t WifiTransport::start(RequestHandler handler, void* context, const char* ap_ssid,
                               const char* ap_password, const WifiCredentials& initial)
{
    if (!handler || !ap_ssid || !ap_password || !credentials_valid_or_disabled(initial))
        return ESP_ERR_INVALID_ARG;
    const size_t ssid_length = strnlen(ap_ssid, 33);
    const size_t password_length = strnlen(ap_password, 64);
    if (ssid_length == 0 || ssid_length > 32 || password_length < 8 || password_length > 63)
        return ESP_ERR_INVALID_ARG;
    if (driver_initialized_ || sta_netif_ || ap_netif_ || http_ || wifi_events_ || ip_events_)
        return ESP_ERR_INVALID_STATE;

    handler_ = handler;
    context_ = context;
    {
        StateGuard guard(state_lock_);
        state_ = {};
        diagnostics_ = {};
        diagnostics_.enabled = true;
        std::memcpy(state_.ap_ssid, ap_ssid, ssid_length);
        std::memcpy(state_.ssid, initial.ssid, sizeof(state_.ssid));
        stopping_ = false;
        station_enabled_ = initial.ssid[0] != '\0';
        diagnostics_.station_enabled = station_enabled_;
        station_started_ = false;
        attempted_connection_ = false;
        next_connect_us_ = 0;
        connect_deadline_us_ = 0;
        last_rssi_us_ = 0;
        scan_in_flight_ = scan_done_pending_ = scan_cancelled_ = false;
        scan_status_ = ESP_OK;
        scan_deadline_us_ = 0;
        backoff_.reset();
        state_.state = station_enabled_ ? WifiState::connecting : WifiState::access_point;
    }

    const esp_err_t result = [&]() -> esp_err_t {
        // The convenience esp_netif_create_default_wifi_* APIs abort on error.
        // Use their fallible constituent operations so AppModule can recover.
        esp_netif_config_t sta_config = ESP_NETIF_DEFAULT_WIFI_STA();
        sta_netif_ = esp_netif_new(&sta_config);
        if (!sta_netif_) return ESP_ERR_NO_MEM;
        sta_attach_attempted_ = true;
        esp_err_t error = esp_netif_attach_wifi_station(sta_netif_);
        if (error != ESP_OK) return error;
        error = esp_wifi_set_default_wifi_sta_handlers();
        if (error != ESP_OK) return error;

        esp_netif_config_t ap_config = ESP_NETIF_DEFAULT_WIFI_AP();
        ap_netif_ = esp_netif_new(&ap_config);
        if (!ap_netif_) return ESP_ERR_NO_MEM;
        ap_attach_attempted_ = true;
        error = esp_netif_attach_wifi_ap(ap_netif_);
        if (error != ESP_OK) return error;
        error = esp_wifi_set_default_wifi_ap_handlers();
        if (error != ESP_OK) return error;
        error = esp_netif_dhcps_stop(ap_netif_);
        if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return error;
        esp_netif_ip_info_t ap_ip{};
        IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
        IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
        IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
        error = esp_netif_set_ip_info(ap_netif_, &ap_ip);
        if (error != ESP_OK) return error;
        // Re-arm automatic DHCP startup after setting the fixed AP address.
        error = esp_netif_dhcps_start(ap_netif_);
        if (error != ESP_OK) return error;

        wifi_init_config_t driver_config = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&driver_config);
        if (error != ESP_OK) return error;
        driver_initialized_ = true;
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (error != ESP_OK) return error;
        // Keep the station interface started for on-demand scans even without
        // credentials. Only process() may initiate a station connection.
        error = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (error != ESP_OK) return error;

        wifi_config_t access_point{};
        std::memcpy(access_point.ap.ssid, ap_ssid, ssid_length);
        access_point.ap.ssid_len = static_cast<uint8_t>(ssid_length);
        std::memcpy(access_point.ap.password, ap_password, password_length);
        access_point.ap.channel = 1;
        access_point.ap.max_connection = 2;
        access_point.ap.beacon_interval = 100;
        access_point.ap.pmf_cfg.capable = true;
        access_point.ap.pmf_cfg.required = false;
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
        access_point.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        access_point.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#else
        access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
#endif
        error = esp_wifi_set_config(WIFI_IF_AP, &access_point);
        if (error != ESP_OK) return error;
        wifi_config_t station = station_config(initial);
        error = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (error != ESP_OK) return error;
        error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    event_handler, this, &wifi_events_);
        if (error != ESP_OK) return error;
        error = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                    event_handler, this, &ip_events_);
        if (error != ESP_OK) return error;
        driver_start_attempted_ = true;
        error = esp_wifi_start();
        if (error != ESP_OK) return error;
        driver_started_ = true;
        // Default minimum modem sleep allows the coexistence arbiter to serve BLE.
        error = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (error != ESP_OK) return error;
        return start_http();
    }();

    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Start failed: %s", esp_err_to_name(result));
        const esp_err_t cleanup = stop();
        if (cleanup != ESP_OK) ESP_LOGE(kTag, "Retained resources after cleanup: %s", esp_err_to_name(cleanup));
        StateGuard guard(state_lock_);
        state_.state = WifiState::failed;
    } else {
        ESP_LOGI(kTag, "Protected AP active; HTTP API port 80, maximum 2 AP clients");
    }
    return result;
}

esp_err_t WifiTransport::start_http()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 2;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    esp_err_t result = httpd_start(&http_, &config);
    if (result != ESP_OK) return result;
    httpd_uri_t status{};
    status.uri = "/api/v1/status";
    status.method = HTTP_GET;
    status.handler = status_handler;
    status.user_ctx = this;
    result = httpd_register_uri_handler(http_, &status);
    if (result != ESP_OK) return result;
    httpd_uri_t command{};
    command.uri = "/api/v1/command";
    command.method = HTTP_POST;
    command.handler = command_handler;
    command.user_ctx = this;
    result = httpd_register_uri_handler(http_, &command);
    if (result != ESP_OK) return result;
    constexpr httpd_err_code_t errors[] = {HTTPD_400_BAD_REQUEST, HTTPD_404_NOT_FOUND,
        HTTPD_405_METHOD_NOT_ALLOWED, HTTPD_408_REQ_TIMEOUT, HTTPD_413_CONTENT_TOO_LARGE};
    for (const auto error : errors) {
        result = httpd_register_err_handler(http_, error, error_handler);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

esp_err_t WifiTransport::stop()
{
    if (driver_started_) {
        const esp_err_t cancel = cancel_scan(ESP_ERR_INVALID_STATE);
        if (cancel != ESP_OK) ESP_LOGD(kTag, "Scan cancellation before shutdown: %s", esp_err_to_name(cancel));
    }
    {
        StateGuard guard(state_lock_);
        stopping_ = true;
        next_connect_us_ = 0;
        connect_deadline_us_ = 0;
    }
    // Each successful step is recorded. On failure, retain the remaining
    // dependencies and handles so a subsequent stop() can retry safely.
    esp_err_t result;
    if (http_) {
        result = httpd_stop(http_);
        if (result != ESP_OK) return result;
        http_ = nullptr;
    }
    if (driver_start_attempted_) {
        result = esp_wifi_stop();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED) return result;
        driver_started_ = false;
        driver_start_attempted_ = false;
    }
    if (ip_events_) {
        result = esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_events_);
        if (result != ESP_OK) return result;
        ip_events_ = nullptr;
    }
    if (wifi_events_) {
        result = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events_);
        if (result != ESP_OK) return result;
        wifi_events_ = nullptr;
    }
    if (driver_initialized_) {
        result = esp_wifi_deinit();
        if (result != ESP_OK) return result;
        driver_initialized_ = false;
    }
    if (ap_netif_) {
        if (ap_attach_attempted_) {
            result = esp_wifi_clear_default_wifi_driver_and_handlers(ap_netif_);
            if (result != ESP_OK) return result;
            ap_attach_attempted_ = false;
        }
        esp_netif_destroy(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (sta_netif_) {
        if (sta_attach_attempted_) {
            result = esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif_);
            if (result != ESP_OK) return result;
            sta_attach_attempted_ = false;
        }
        esp_netif_destroy(sta_netif_);
        sta_netif_ = nullptr;
    }
    handler_ = nullptr;
    context_ = nullptr;
    {
        StateGuard guard(state_lock_);
        state_ = {};
        diagnostics_ = {};
        scan_in_flight_ = scan_done_pending_ = scan_cancelled_ = false;
        scan_deadline_us_ = 0;
        scan_status_ = ESP_OK;
        station_enabled_ = false;
        station_started_ = false;
        attempted_connection_ = false;
        backoff_.reset();
    }
    return ESP_OK;
}

esp_err_t WifiTransport::configuration_ready()
{
    if (!driver_initialized_) return ESP_ERR_INVALID_STATE;
    StateGuard guard(state_lock_);
    if (stopping_) return ESP_ERR_INVALID_STATE;
    if (!driver_started_) {
        // A fully stopped radio may update its RAM settings without enabling
        // RF, including while an already-cancelled SCAN_DONE is being drained.
        return !diagnostics_.enabled && !driver_start_attempted_
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!diagnostics_.enabled) return ESP_ERR_INVALID_STATE;
    return scan_in_flight_ ? ESP_ERR_WIFI_STATE : ESP_OK;
}

esp_err_t WifiTransport::configure(const WifiCredentials& credentials)
{
    if (!credentials_valid_or_disabled(credentials)) return ESP_ERR_INVALID_ARG;
    if (!driver_initialized_) return ESP_ERR_INVALID_STATE;
    if (!driver_started_) {
        {
            StateGuard guard(state_lock_);
            if (stopping_ || diagnostics_.enabled || driver_start_attempted_)
                return ESP_ERR_INVALID_STATE;
        }
        // A stopped, initialized driver accepts configuration when STA is in
        // its selected mode. Keep RF off: clearing credentials must also clear
        // the RAM driver configuration before a later set_enabled(true).
        esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (result == ESP_OK) {
            wifi_config_t config = station_config(credentials);
            result = esp_wifi_set_config(WIFI_IF_STA, &config);
        }
        if (result == ESP_OK) {
            StateGuard guard(state_lock_);
            station_enabled_ = credentials.ssid[0] != '\0';
            diagnostics_.station_enabled = false;
            station_started_ = attempted_connection_ = false;
            next_connect_us_ = connect_deadline_us_ = 0;
            state_.state = WifiState::stopped;
            std::memcpy(state_.ssid, credentials.ssid, sizeof(state_.ssid));
            state_.address[0] = '\0';
            state_.rssi = 0;
            state_.disconnect_reason = 0;
            diagnostics_.bssid[0] = '\0';
            backoff_.reset();
        }
        if (result != ESP_OK) ESP_LOGE(kTag, "Stopped STA configuration failed: %s", esp_err_to_name(result));
        return result;
    }
    {
        StateGuard guard(state_lock_);
        if (stopping_ || !diagnostics_.enabled) return ESP_ERR_INVALID_STATE;
        if (scan_in_flight_) return ESP_ERR_WIFI_STATE;
        station_enabled_ = false;
        diagnostics_.station_enabled = false;
        station_started_ = false;
        attempted_connection_ = false;
        next_connect_us_ = 0;
        connect_deadline_us_ = 0;
        state_.state = WifiState::access_point;
        state_.ssid[0] = '\0';
        state_.address[0] = '\0';
        state_.rssi = 0;
        state_.disconnect_reason = 0;
        diagnostics_.bssid[0] = '\0';
        backoff_.reset();
    }
    // Cycling only STA cancels association/DHCP while AP clients retain their
    // interface. Restore idle APSTA even for empty credentials so scans work.
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) {
        wifi_config_t config = station_config(credentials);
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    const int64_t now = esp_timer_get_time();
    {
        StateGuard guard(state_lock_);
        if (result == ESP_OK) {
            station_enabled_ = credentials.ssid[0] != '\0';
            diagnostics_.station_enabled = station_enabled_;
            std::memcpy(state_.ssid, credentials.ssid, sizeof(state_.ssid));
            state_.state = station_enabled_ ? WifiState::connecting : WifiState::access_point;
            if (station_enabled_ && station_started_) next_connect_us_ = now;
        } else {
            state_.state = WifiState::failed;
        }
    }
    if (result != ESP_OK) ESP_LOGE(kTag, "STA configuration failed: %s", esp_err_to_name(result));
    return result;
}

esp_err_t WifiTransport::disconnect_station()
{
    if (!driver_started_) return ESP_ERR_INVALID_STATE;
    {
        StateGuard guard(state_lock_);
        if (stopping_ || !diagnostics_.enabled) return ESP_ERR_INVALID_STATE;
        if (scan_in_flight_) return ESP_ERR_WIFI_STATE;
        // Preserve credentials and SSID. Only configure() re-arms connection;
        // a later radio off/on must not undo an explicit STA disconnect.
        station_enabled_ = false;
        diagnostics_.station_enabled = false;
        next_connect_us_ = connect_deadline_us_ = 0;
        state_.state = WifiState::access_point;
        state_.address[0] = '\0';
        state_.rssi = 0;
        state_.disconnect_reason = 0;
        diagnostics_.bssid[0] = '\0';
        backoff_.reset();
    }
    esp_err_t result = esp_wifi_disconnect();
    if (result == ESP_ERR_WIFI_NOT_CONNECT) result = ESP_OK;
    if (result != ESP_OK) {
        StateGuard guard(state_lock_);
        state_.state = WifiState::failed;
    }
    return result;
}

esp_err_t WifiTransport::set_enabled(bool enabled)
{
    if (!driver_initialized_ || !http_) return ESP_ERR_INVALID_STATE;
    {
        StateGuard guard(state_lock_);
        if (stopping_) return ESP_ERR_INVALID_STATE;
        if (enabled == diagnostics_.enabled && enabled == driver_started_) return ESP_OK;
    }
    if (!enabled) {
        // Do not unregister handlers or stop HTTP: BLE/local UI can re-enable
        // the same radio resources. In-flight HTTP sessions lose their link.
        const esp_err_t cancel = cancel_scan(ESP_ERR_INVALID_STATE);
        if (cancel != ESP_OK) ESP_LOGW(kTag, "Scan cancellation before radio off: %s", esp_err_to_name(cancel));
        {
            StateGuard guard(state_lock_);
            diagnostics_.enabled = false;
            diagnostics_.station_enabled = false;
            next_connect_us_ = connect_deadline_us_ = 0;
        }
        const esp_err_t result = esp_wifi_stop();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED) {
            StateGuard guard(state_lock_);
            diagnostics_.enabled = true;
            diagnostics_.station_enabled = station_enabled_;
            state_.state = WifiState::failed;
            return result;
        }
        driver_started_ = driver_start_attempted_ = false;
        {
            StateGuard guard(state_lock_);
            station_started_ = false;
            state_.state = WifiState::stopped;
            state_.ap_active = false;
            state_.ap_address[0] = '\0';
            state_.address[0] = '\0';
            state_.rssi = 0;
            diagnostics_.bssid[0] = '\0';
            diagnostics_.channel = diagnostics_.ap_clients = 0;
        }
        return ESP_OK;
    }
    {
        StateGuard guard(state_lock_);
        diagnostics_.enabled = true; // Allow the asynchronous START events.
        diagnostics_.station_enabled = station_enabled_;
        station_started_ = false;
        attempted_connection_ = false;
        next_connect_us_ = connect_deadline_us_ = 0;
        last_rssi_us_ = 0;
        backoff_.reset();
        state_.state = station_enabled_ ? WifiState::connecting : WifiState::access_point;
        state_.disconnect_reason = 0;
    }
    driver_start_attempted_ = true;
    esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) {
        const esp_err_t cleanup = esp_wifi_stop();
        const bool retained = cleanup != ESP_OK && cleanup != ESP_ERR_WIFI_NOT_STARTED;
        driver_started_ = false;
        driver_start_attempted_ = retained;
        StateGuard guard(state_lock_);
        diagnostics_.enabled = retained;
        diagnostics_.station_enabled = false;
        state_.state = WifiState::failed;
        return result;
    }
    driver_started_ = true;
    result = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return result;
}

esp_err_t WifiTransport::request_scan()
{
    if (!driver_started_) return ESP_ERR_INVALID_STATE;
    {
        StateGuard guard(state_lock_);
        if (stopping_ || !diagnostics_.enabled || !station_started_) return ESP_ERR_INVALID_STATE;
        if (scan_in_flight_ || (station_enabled_ && state_.state == WifiState::connecting))
            return ESP_ERR_WIFI_STATE;
        scan_in_flight_ = true;
        scan_done_pending_ = scan_cancelled_ = false;
        scan_status_ = ESP_OK;
        scan_deadline_us_ = esp_timer_get_time() + kScanTimeoutUs;
        diagnostics_.scanning = true;
        diagnostics_.scan_error = ESP_OK;
    }
    wifi_scan_config_t config{};
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // Request SDK default active timings. wifi.rst specifies min=max=0
    // as the default 120 ms dwell. The coexistence driver requires default
    // active timings when Bluetooth shares the radio.
    config.scan_time.active.min = 0;
    config.scan_time.active.max = 0;
    // Give AP clients and BLE regular home-channel service opportunities.
    // Foreground/background selection otherwise belongs to the Wi-Fi driver.
    config.home_chan_dwell_time = 100;
    config.coex_background_scan = true;
    const esp_err_t result = esp_wifi_scan_start(&config, false);
    if (result != ESP_OK) {
        StateGuard guard(state_lock_);
        scan_in_flight_ = scan_done_pending_ = false;
        scan_deadline_us_ = 0;
        diagnostics_.scanning = false;
        diagnostics_.scan_error = result;
        ++diagnostics_.generation;
    }
    return result;
}

esp_err_t WifiTransport::cancel_scan(esp_err_t reason)
{
    bool stop_scan = false;
    {
        StateGuard guard(state_lock_);
        if (!scan_in_flight_ || scan_cancelled_) return ESP_OK;
        scan_cancelled_ = true;
        scan_status_ = reason;
        diagnostics_.scanning = false;
        diagnostics_.scan_error = reason;
        stop_scan = !scan_done_pending_;
    }
    // IDF posts SCAN_DONE for scan_stop as well as normal completion. Keep the
    // in-flight marker until process consumes it, preventing stale completions.
    return stop_scan ? esp_wifi_scan_stop() : ESP_OK;
}

void WifiTransport::finish_scan(esp_err_t status)
{
    wifi_ap_record_t records[kMaxWifiNetworks]{};
    WifiNetwork networks[kMaxWifiNetworks]{};
    uint16_t total = 0;
    uint16_t count = static_cast<uint16_t>(kMaxWifiNetworks);
    if (status == ESP_OK) {
        status = esp_wifi_scan_get_ap_num(&total);
        if (status == ESP_OK && total != 0) {
            status = esp_wifi_scan_get_ap_records(&count, records);
        } else {
            count = 0;
            const esp_err_t clear = esp_wifi_clear_ap_list();
            if (status == ESP_OK) status = clear;
        }
    }
    if (status != ESP_OK) {
        // scan_get_ap_records frees the entire driver list on success, even
        // when truncated to 16. Every error/cancellation also releases it.
        const esp_err_t clear = esp_wifi_clear_ap_list();
        if (clear != ESP_OK && clear != ESP_ERR_WIFI_NOT_STARTED)
            ESP_LOGD(kTag, "Scan result cleanup: %s", esp_err_to_name(clear));
    } else {
        count = std::min(count, static_cast<uint16_t>(kMaxWifiNetworks));
        for (uint16_t i = 0; i < count; ++i) {
            std::memcpy(networks[i].ssid, records[i].ssid, sizeof(records[i].ssid));
            networks[i].ssid[sizeof(networks[i].ssid) - 1] = '\0';
            format_bssid(networks[i].bssid, records[i].bssid);
            networks[i].rssi = records[i].rssi;
            networks[i].channel = records[i].primary;
            networks[i].auth = static_cast<uint8_t>(records[i].authmode);
        }
        // IDF already returns its strongest APs first; keep deterministic order
        // for equal RSSI so a refreshed menu does not jump unnecessarily.
        std::sort(networks, networks + count, [](const WifiNetwork& left, const WifiNetwork& right) {
            return left.rssi != right.rssi ? left.rssi > right.rssi
                : std::strcmp(left.bssid, right.bssid) < 0;
        });
    }
    {
        StateGuard guard(state_lock_);
        if (status == ESP_OK) {
            diagnostics_.total_found = total;
            diagnostics_.count = static_cast<uint8_t>(count);
            std::memcpy(diagnostics_.networks, networks, sizeof(networks));
        }
        diagnostics_.scan_error = status;
        diagnostics_.scanning = false;
        ++diagnostics_.generation;
        scan_in_flight_ = scan_done_pending_ = scan_cancelled_ = false;
        scan_deadline_us_ = 0;
    }
    if (status == ESP_OK) ESP_LOGI(kTag, "Scan complete: %u networks, showing %u", total, count);
    else ESP_LOGW(kTag, "Scan ended: %s", esp_err_to_name(status));
}

void WifiTransport::schedule_retry_locked(int64_t now_us)
{
    if (!station_enabled_ || stopping_ || !diagnostics_.enabled || state_.state == WifiState::retry_wait) return;
    state_.state = WifiState::retry_wait;
    state_.address[0] = '\0';
    state_.rssi = 0;
    diagnostics_.bssid[0] = '\0';
    connect_deadline_us_ = 0;
    next_connect_us_ = now_us + static_cast<int64_t>(backoff_.next_delay_ms()) * 1000;
}

void WifiTransport::process()
{
    const int64_t now = esp_timer_get_time();
    bool scan_done = false;
    bool scan_timeout = false;
    esp_err_t scan_status = ESP_OK;
    {
        StateGuard guard(state_lock_);
        if (stopping_) return;
        scan_done = scan_in_flight_ && scan_done_pending_;
        scan_status = scan_status_;
        scan_timeout = scan_in_flight_ && !scan_cancelled_ &&
            !scan_done && now >= scan_deadline_us_;
    }
    if (scan_timeout) {
        const esp_err_t result = cancel_scan(ESP_ERR_TIMEOUT);
        if (result != ESP_OK) ESP_LOGW(kTag, "Scan timeout cancellation: %s", esp_err_to_name(result));
    }
    if (scan_done) finish_scan(scan_status);

    bool connect = false;
    bool disconnect = false;
    bool sample_diagnostics = false;
    bool station_connected = false;
    {
        StateGuard guard(state_lock_);
        if (!diagnostics_.enabled || !driver_started_) return;
        // Scanner and the connect retry machine share the STA radio. Defer
        // retries while scanning; a scan never interrupts an active attempt.
        if (station_enabled_ && station_started_ && !scan_in_flight_) {
            if (next_connect_us_ != 0 && now >= next_connect_us_) {
                next_connect_us_ = 0;
                connect_deadline_us_ = now + kConnectTimeoutUs;
                state_.state = WifiState::connecting;
                if (attempted_connection_ && state_.reconnects < std::numeric_limits<uint32_t>::max())
                    ++state_.reconnects;
                attempted_connection_ = true;
                connect = true;
            } else if (connect_deadline_us_ != 0 && now >= connect_deadline_us_) {
                schedule_retry_locked(now);
                disconnect = true;
            }
        }
        if (!scan_in_flight_ && now - last_rssi_us_ >= kRssiIntervalUs) {
            last_rssi_us_ = now;
            sample_diagnostics = true;
            station_connected = state_.state == WifiState::connected;
        }
    }
    if (disconnect) {
        const esp_err_t result = esp_wifi_disconnect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT)
            ESP_LOGW(kTag, "Disconnect after connection deadline: %s", esp_err_to_name(result));
    }
    if (connect) {
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Connection request failed: %s", esp_err_to_name(result));
            StateGuard guard(state_lock_);
            schedule_retry_locked(now);
        }
    }
    if (sample_diagnostics) {
        uint8_t channel = 0;
        wifi_second_chan_t secondary{};
        const esp_err_t channel_result = esp_wifi_get_channel(&channel, &secondary);
        wifi_sta_list_t clients{};
        const esp_err_t clients_result = esp_wifi_ap_get_sta_list(&clients);
        wifi_ap_record_t ap{};
        const esp_err_t ap_result = station_connected
            ? esp_wifi_sta_get_ap_info(&ap) : ESP_ERR_WIFI_NOT_CONNECT;
        char bssid[18]{};
        if (ap_result == ESP_OK) format_bssid(bssid, ap.bssid);
        StateGuard guard(state_lock_);
        if (diagnostics_.enabled) {
            if (channel_result == ESP_OK) diagnostics_.channel = channel;
            if (clients_result == ESP_OK)
                diagnostics_.ap_clients = static_cast<uint8_t>(clients.num);
            if (state_.state == WifiState::connected && ap_result == ESP_OK) {
                state_.rssi = ap.rssi;
                diagnostics_.channel = ap.primary;
                std::memcpy(diagnostics_.bssid, bssid, sizeof(bssid));
            }
        }
    }
}

WifiSnapshot WifiTransport::snapshot()
{
    StateGuard guard(state_lock_);
    return state_;
}

WifiDiagnostics WifiTransport::diagnostics()
{
    StateGuard guard(state_lock_);
    return diagnostics_;
}

void WifiTransport::event_handler(void* context, esp_event_base_t base, int32_t id, void* data)
{
    static_cast<WifiTransport*>(context)->on_event(base, id, data);
}

void WifiTransport::on_event(esp_event_base_t base, int32_t id, void* data)
{
    const int64_t now = esp_timer_get_time();
    char address[16]{};
    char bssid[18]{};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        if (event->esp_netif != sta_netif_) return;
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && data) {
        format_bssid(bssid, static_cast<const wifi_event_sta_connected_t*>(data)->bssid);
    }
    StateGuard guard(state_lock_);
    if (stopping_) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (scan_in_flight_) {
            scan_done_pending_ = true;
            if (!scan_cancelled_) {
                scan_status_ = data && static_cast<const wifi_event_sta_scan_done_t*>(data)->status == 0
                    ? ESP_OK : ESP_FAIL;
            }
        }
        return; // The owner processes and frees results, including cancelled scans.
    }
    if (!diagnostics_.enabled) return;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            state_.ap_active = true;
            std::memcpy(state_.ap_address, "192.168.4.1", sizeof("192.168.4.1"));
            if (diagnostics_.channel == 0) diagnostics_.channel = 1;
            break;
        case WIFI_EVENT_AP_STOP:
            state_.ap_active = false;
            state_.ap_address[0] = '\0';
            diagnostics_.ap_clients = 0;
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            if (diagnostics_.ap_clients < 2) ++diagnostics_.ap_clients;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (diagnostics_.ap_clients != 0) --diagnostics_.ap_clients;
            break;
        case WIFI_EVENT_HOME_CHANNEL_CHANGE:
            if (data) diagnostics_.channel =
                static_cast<const wifi_event_home_channel_change_t*>(data)->new_chan;
            break;
        case WIFI_EVENT_STA_START:
            station_started_ = true;
            if (station_enabled_) {
                state_.state = WifiState::connecting;
                next_connect_us_ = now;
            }
            break;
        case WIFI_EVENT_STA_STOP:
            station_started_ = false;
            next_connect_us_ = 0;
            connect_deadline_us_ = 0;
            state_.address[0] = '\0';
            state_.rssi = 0;
            diagnostics_.bssid[0] = '\0';
            state_.state = station_enabled_ ? WifiState::connecting : WifiState::access_point;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            if (station_enabled_ && station_started_ && data) {
                diagnostics_.channel = static_cast<const wifi_event_sta_connected_t*>(data)->channel;
                std::memcpy(diagnostics_.bssid, bssid, sizeof(bssid));
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (station_enabled_ && station_started_) {
                if (data) state_.disconnect_reason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
                schedule_retry_locked(now);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && station_enabled_ && station_started_) {
        if (id == IP_EVENT_STA_GOT_IP && data) {
            std::memcpy(state_.address, address, sizeof(state_.address));
            state_.state = WifiState::connected;
            state_.disconnect_reason = 0;
            next_connect_us_ = 0;
            connect_deadline_us_ = 0;
            last_rssi_us_ = now - kRssiIntervalUs;
            backoff_.reset();
        } else if (id == IP_EVENT_STA_LOST_IP && state_.state == WifiState::connected) {
            state_.state = WifiState::connecting;
            state_.address[0] = '\0';
            connect_deadline_us_ = now + kConnectTimeoutUs;
        }
    }
}

esp_err_t WifiTransport::dispatch(httpd_req_t* request, const char* body, size_t length)
{
    {
        StateGuard guard(state_lock_);
        if (stopping_) return ESP_FAIL;
    }
    char response[kMaxResponseBytes]{};
    handler_(context_, body, length, response, sizeof(response));
    response[sizeof(response) - 1] = '\0';
    if (response[0] == '\0')
        return reject_request(request, "500 Internal Server Error", "empty_response");
    return send_json(request, "200 OK", response, std::strlen(response));
}

esp_err_t WifiTransport::status_handler(httpd_req_t* request)
{
    if (request->content_len != 0)
        return reject_request(request, "400 Bad Request", "unexpected_body");
    constexpr char body[] = "{\"v\":1,\"id\":0,\"op\":\"status\"}";
    return static_cast<WifiTransport*>(request->user_ctx)->dispatch(request, body, sizeof(body) - 1);
}

esp_err_t WifiTransport::command_handler(httpd_req_t* request)
{
    if (request->content_len > kMaxRequestBytes)
        return reject_request(request, "413 Content Too Large", "request_too_large");
    if (request->content_len == 0)
        return reject_request(request, "400 Bad Request", "empty_request");
    if (!json_content_type(request))
        return reject_request(request, "415 Unsupported Media Type", "json_content_type_required");
    char body[kMaxRequestBytes + 1]{};
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + kBodyTimeoutUs;
    while (received < request->content_len) {
        if (esp_timer_get_time() >= deadline)
            return reject_request(request, "408 Request Timeout", "request_timeout", false);
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT)
            return reject_request(request, "408 Request Timeout", "request_timeout", false);
        if (count <= 0) return reject_request(request, "400 Bad Request", "incomplete_request", false);
        received += static_cast<size_t>(count);
    }
    if (std::memchr(body, '\0', received))
        return reject_request(request, "400 Bad Request", "embedded_nul");
    return static_cast<WifiTransport*>(request->user_ctx)->dispatch(request, body, received);
}

esp_err_t WifiTransport::error_handler(httpd_req_t* request, httpd_err_code_t error)
{
    switch (error) {
    case HTTPD_404_NOT_FOUND: return reject_request(request, "404 Not Found", "not_found");
    case HTTPD_405_METHOD_NOT_ALLOWED: return reject_request(request, "405 Method Not Allowed", "method_not_allowed");
    case HTTPD_408_REQ_TIMEOUT: return reject_request(request, "408 Request Timeout", "request_timeout", false);
    case HTTPD_413_CONTENT_TOO_LARGE: return reject_request(request, "413 Content Too Large", "request_too_large");
    default: return reject_request(request, "400 Bad Request", "bad_request");
    }
}

#else

WifiTransport::~WifiTransport() = default;
esp_err_t WifiTransport::start(RequestHandler, void*, const char*, const char*, const WifiCredentials&)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t WifiTransport::stop() { return ESP_OK; }
void WifiTransport::process() {}
esp_err_t WifiTransport::configuration_ready() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t WifiTransport::configure(const WifiCredentials&) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t WifiTransport::request_scan() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t WifiTransport::set_enabled(bool) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t WifiTransport::disconnect_station() { return ESP_ERR_NOT_SUPPORTED; }
WifiSnapshot WifiTransport::snapshot() { return {}; }
WifiDiagnostics WifiTransport::diagnostics() { return {}; }

#endif
} // namespace connectivity
