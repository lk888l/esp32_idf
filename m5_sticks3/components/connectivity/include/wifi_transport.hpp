#pragma once

#include "connectivity_policy.hpp"
#include "connectivity_types.hpp"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

namespace connectivity {

// Lifecycle and configuration belong to the AppModule owner task. IDF event
// and HTTP tasks only publish bounded state or invoke the request dispatcher.
class WifiTransport final {
public:
    WifiTransport() = default;
    ~WifiTransport();

    WifiTransport(const WifiTransport&) = delete;
    WifiTransport& operator=(const WifiTransport&) = delete;

    // The owner initializes NVS, esp_netif and the default event loop first.
    // Empty STA credentials select AP-only mode; the password-protected AP
    // remains available when a configured station connects or loses its AP.
    esp_err_t start(RequestHandler handler, void* context, const char* ap_ssid,
                    const char* ap_password, const WifiCredentials& initial);
    esp_err_t stop();
    void process();
    esp_err_t configure(const WifiCredentials& credentials);
    WifiSnapshot snapshot();

private:
    static void event_handler(void* context, esp_event_base_t base,
                              int32_t id, void* data);
    static esp_err_t status_handler(httpd_req_t* request);
    static esp_err_t command_handler(httpd_req_t* request);
    static esp_err_t error_handler(httpd_req_t* request, httpd_err_code_t error);
    esp_err_t start_http();
    esp_err_t dispatch(httpd_req_t* request, const char* body, size_t length);
    void on_event(esp_event_base_t base, int32_t id, void* data);
    void schedule_retry_locked(int64_t now_us);

    // These resources are only changed by the owner task. httpd_stop and event
    // unregistration synchronize producers before their dependencies are freed.
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    bool sta_attach_attempted_ = false;
    bool ap_attach_attempted_ = false;
    bool driver_initialized_ = false;
    bool driver_started_ = false;
    bool driver_start_attempted_ = false;
    esp_event_handler_instance_t wifi_events_ = nullptr;
    esp_event_handler_instance_t ip_events_ = nullptr;
    httpd_handle_t http_ = nullptr;
    RequestHandler handler_ = nullptr;
    void* context_ = nullptr;

    portMUX_TYPE state_lock_ = portMUX_INITIALIZER_UNLOCKED;
    WifiSnapshot state_{};
    RetryBackoff backoff_{};
    bool stopping_ = true;
    bool station_enabled_ = false;
    bool station_started_ = false;
    bool attempted_connection_ = false;
    int64_t next_connect_us_ = 0;
    int64_t connect_deadline_us_ = 0;
    int64_t last_rssi_us_ = 0;
};

} // namespace connectivity