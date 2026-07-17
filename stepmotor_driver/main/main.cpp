#include "app_manager.hpp"
#include "foc_demo_module.hpp"
#include "logger.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    static app_modules::FocDemoModule foc_demo;

    logger::Logger log{"main"};
    auto& manager = app::AppManager::instance();

    if (!manager.registerModule(foc_demo)) {
        log.error("failed to register FOC demo module");
        return;
    }

    if (!manager.initializeAll()) {
        log.error("application initialization failed; motor outputs remain disabled");
        return;
    }

    log.info("stepmotor_driver started");
    while (true) {
        manager.processAll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
