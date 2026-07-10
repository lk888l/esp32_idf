#include "app_manager.hpp"
#include "gyro_reader_module.hpp"
#include "logger.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    ium::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(app_modules::createGyroReaderModule())) {
        log.error("failed to register gyro reader module");
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

    log.info("application started");

    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
