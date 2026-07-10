#pragma once

#include "app_module.hpp"
#include "esp_camera_device.hpp"
#include "logger.hpp"

#include <string_view>

#include "esp_http_server.h"

namespace camera_web {

class CameraWebServerModule final : public AppModule {
public:
    bool initialize() override;
    bool deinitialize() override;
    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "camera_web_server"; }

private:
    static esp_err_t indexHandler(httpd_req_t* req);
    static esp_err_t captureHandler(httpd_req_t* req);
    static esp_err_t streamHandler(httpd_req_t* req);
    static esp_err_t statusHandler(httpd_req_t* req);

    bool startServer();
    void stopServer();

    ium::Logger log_{"camera_web"};
    camera::EspCameraDevice camera_;
    httpd_handle_t server_ = nullptr;
    bool initialized_ = false;
};

} // namespace camera_web
