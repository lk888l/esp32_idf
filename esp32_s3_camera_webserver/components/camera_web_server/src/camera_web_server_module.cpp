#include "camera_web_server_module.hpp"

#include <cstdio>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kStreamContentType[] = "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";

constexpr char kIndexHtml[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-S3 Camera</title>
  <style>
    body { margin: 0; font-family: system-ui, sans-serif; background: #111; color: #eee; }
    main { margin: 0; padding: 20px; }
    h1 { font-size: 22px; font-weight: 650; }
    img { width: auto; max-width: none; height: auto; background: #222; display: block; }
    a { color: #8fd3ff; }
  </style>
</head>
<body>
  <main>
    <h1>ESP32-S3 Camera Web Server</h1>
    <p><a href="/capture">Capture JPEG</a> | <a href="/status">Status</a></p>
    <img src="/stream" alt="camera stream">
  </main>
</body>
</html>
)HTML";

camera_web::CameraWebServerModule* moduleFromRequest(httpd_req_t* req)
{
    return static_cast<camera_web::CameraWebServerModule*>(req->user_ctx);
}

} // namespace

namespace camera_web {

bool CameraWebServerModule::initialize()
{
    if (initialized_) {
        return true;
    }

    if (!camera_.initialize()) {
        return false;
    }

    if (!startServer()) {
        camera_.deinitialize();
        return false;
    }

    initialized_ = true;
    return true;
}

bool CameraWebServerModule::deinitialize()
{
    if (!initialized_) {
        return true;
    }

    stopServer();
    const bool camera_ok = camera_.deinitialize();
    initialized_ = false;
    return camera_ok;
}

bool CameraWebServerModule::startServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = CONFIG_CAMERA_WEB_HTTPD_STACK_SIZE;
    config.ctrl_port = 32768;

    const esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        log_.error("httpd_start failed: 0x{:x}", static_cast<int>(ret));
        return false;
    }

    httpd_uri_t index_uri{};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = &CameraWebServerModule::indexHandler;
    index_uri.user_ctx = this;

    httpd_uri_t capture_uri{};
    capture_uri.uri = "/capture";
    capture_uri.method = HTTP_GET;
    capture_uri.handler = &CameraWebServerModule::captureHandler;
    capture_uri.user_ctx = this;

    httpd_uri_t stream_uri{};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = &CameraWebServerModule::streamHandler;
    stream_uri.user_ctx = this;

    httpd_uri_t status_uri{};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = &CameraWebServerModule::statusHandler;
    status_uri.user_ctx = this;

    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &capture_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &stream_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &status_uri));

    log_.info("HTTP server started on port {}", config.server_port);
    return true;
}

void CameraWebServerModule::stopServer()
{
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

esp_err_t CameraWebServerModule::indexHandler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t CameraWebServerModule::captureHandler(httpd_req_t* req)
{
    auto* self = moduleFromRequest(req);
    if (self == nullptr) {
        return ESP_FAIL;
    }

    camera_fb_t* frame = self->camera_.capture();
    if (frame == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    const esp_err_t ret = httpd_resp_send(req,
                                          reinterpret_cast<const char*>(frame->buf),
                                          frame->len);
    self->camera_.release(frame);
    return ret;
}

esp_err_t CameraWebServerModule::streamHandler(httpd_req_t* req)
{
    auto* self = moduleFromRequest(req);
    if (self == nullptr) {
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_set_type(req, kStreamContentType);
    if (ret != ESP_OK) {
        return ret;
    }

    char header[96]{};

    while (true) {
        camera_fb_t* frame = self->camera_.capture();
        if (frame == nullptr) {
            self->log_.warn("failed to capture frame for stream");
            return ESP_FAIL;
        }

        const int header_len = std::snprintf(
            header,
            sizeof(header),
            "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lld\r\n\r\n",
            static_cast<unsigned>(frame->len),
            static_cast<long long>(esp_timer_get_time()));

        ret = httpd_resp_send_chunk(req, kStreamBoundary, sizeof(kStreamBoundary) - 1);
        if (ret == ESP_OK && header_len > 0) {
            ret = httpd_resp_send_chunk(req, header, header_len);
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(req,
                                        reinterpret_cast<const char*>(frame->buf),
                                        frame->len);
        }

        self->camera_.release(frame);

        if (ret != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ret;
}

esp_err_t CameraWebServerModule::statusHandler(httpd_req_t* req)
{
    auto* self = moduleFromRequest(req);
    if (self == nullptr) {
        return ESP_FAIL;
    }

    char body[128]{};
    const int len = std::snprintf(body,
                                  sizeof(body),
                                  "{\"camera_initialized\":%s,\"stream\":\"/stream\",\"capture\":\"/capture\"}",
                                  self->camera_.is_initialized() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

} // namespace camera_web
