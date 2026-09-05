#include "wifi_transport.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

namespace connectivity {
namespace {
constexpr char kTag[] = "WifiTransport";
constexpr int64_t kConnectTimeoutUs = 30'000'000;
constexpr int64_t kBodyTimeoutUs = 2'000'000;
constexpr int64_t kRssiIntervalUs = 5'000'000;

class StateGuard final {
public:
    explicit StateGuard(portMUX_TYPE& lock) : lock_(lock) { portENTER_CRITICAL(&lock_); }
    ~StateGuard() { portEXIT_CRITICAL(&lock_); }
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;
private:
    portMUX_TYPE& lock_;
};

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
        std::memcpy(state_.ap_ssid, ap_ssid, ssid_length);
        std::memcpy(state_.ssid, initial.ssid, sizeof(state_.ssid));
        stopping_ = false;
        station_enabled_ = initial.ssid[0] != '\0';
        station_started_ = false;
        attempted_connection_ = false;
        next_connect_us_ = 0;
        connect_deadline_us_ = 0;
        last_rssi_us_ = 0;
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
        error = esp_wifi_set_mode(initial.ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP);
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
        if (initial.ssid[0]) {
            wifi_config_t station = station_config(initial);
            error = esp_wifi_set_config(WIFI_IF_STA, &station);
            if (error != ESP_OK) return error;
        }
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
        station_enabled_ = false;
        station_started_ = false;
        attempted_connection_ = false;
        backoff_.reset();
    }
    return ESP_OK;
}

esp_err_t WifiTransport::configure(const WifiCredentials& credentials)
{
    if (!credentials_valid_or_disabled(credentials)) return ESP_ERR_INVALID_ARG;
    if (!driver_started_) return ESP_ERR_INVALID_STATE;
    {
        StateGuard guard(state_lock_);
        if (stopping_) return ESP_ERR_INVALID_STATE;
        station_enabled_ = false;
        station_started_ = false;
        next_connect_us_ = 0;
        connect_deadline_us_ = 0;
        state_.state = WifiState::access_point;
        state_.ssid[0] = '\0';
        state_.address[0] = '\0';
        state_.rssi = 0;
        state_.disconnect_reason = 0;
        backoff_.reset();
    }
    // Disabling just STA cancels association/DHCP before replacing credentials,
    // while AP clients and the HTTP server retain their interface and sessions.
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK && credentials.ssid[0]) {
        result = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (result == ESP_OK) {
            wifi_config_t config = station_config(credentials);
            result = esp_wifi_set_config(WIFI_IF_STA, &config);
        }
    }
    const int64_t now = esp_timer_get_time();
    {
        StateGuard guard(state_lock_);
        if (result == ESP_OK) {
            station_enabled_ = credentials.ssid[0] != '\0';
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

void WifiTransport::schedule_retry_locked(int64_t now_us)
{
    if (!station_enabled_ || stopping_ || state_.state == WifiState::retry_wait) return;
    state_.state = WifiState::retry_wait;
    state_.address[0] = '\0';
    state_.rssi = 0;
    connect_deadline_us_ = 0;
    next_connect_us_ = now_us + static_cast<int64_t>(backoff_.next_delay_ms()) * 1000;
}

void WifiTransport::process()
{
    const int64_t now = esp_timer_get_time();
    bool connect = false;
    bool disconnect = false;
    bool sample_rssi = false;
    {
        StateGuard guard(state_lock_);
        if (stopping_ || !station_enabled_ || !station_started_) return;
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
        if (state_.state == WifiState::connected && now - last_rssi_us_ >= kRssiIntervalUs) {
            last_rssi_us_ = now;
            sample_rssi = true;
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
    if (sample_rssi) {
        wifi_ap_record_t ap{};
        const esp_err_t result = esp_wifi_sta_get_ap_info(&ap);
        if (result == ESP_OK) {
            StateGuard guard(state_lock_);
            if (state_.state == WifiState::connected) state_.rssi = ap.rssi;
        } else if (result != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGD(kTag, "RSSI unavailable: %s", esp_err_to_name(result));
        }
    }
}

WifiSnapshot WifiTransport::snapshot()
{
    StateGuard guard(state_lock_);
    return state_;
}

void WifiTransport::event_handler(void* context, esp_event_base_t base, int32_t id, void* data)
{
    static_cast<WifiTransport*>(context)->on_event(base, id, data);
}

void WifiTransport::on_event(esp_event_base_t base, int32_t id, void* data)
{
    const int64_t now = esp_timer_get_time();
    char address[16]{};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        if (event->esp_netif != sta_netif_) return;
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
    }
    StateGuard guard(state_lock_);
    if (stopping_) return;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            state_.ap_active = true;
            std::memcpy(state_.ap_address, "192.168.4.1", sizeof("192.168.4.1"));
            break;
        case WIFI_EVENT_AP_STOP:
            state_.ap_active = false;
            state_.ap_address[0] = '\0';
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
            state_.state = station_enabled_ ? WifiState::connecting : WifiState::access_point;
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

} // namespace connectivity