#include "app_manager.hpp"
#include "camera_web_server_module.hpp"
#include "logger.hpp"
#include "wifi_station_module.hpp"

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    ium::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(std::make_unique<wifi::WiFiStationModule>()) ||
        !manager.register_module(std::make_unique<camera_web::CameraWebServerModule>())) {
        log.error("failed to register application modules");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!manager.initialize_all()) {
        log.error("failed to initialize application modules");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    log.info("camera web server application started");

    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
