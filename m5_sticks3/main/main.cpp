#include "app_manager.hpp"
#include "app_modules.hpp"
#include "button_event_bus.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "sleep_service.hpp"

#include <memory>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    constexpr char tag[] = "main";
    sleep_service::restore_after_wake();
    app::ButtonEventBus button_events;
    app::AppManager manager;

    const auto register_module = [&](std::unique_ptr<AppModule> module) {
        const app::RegistrationResult result = manager.register_module(std::move(module));
        if (!result) {
            const std::string_view module_name =
                result.module_name.empty() ? std::string_view{"<unnamed>"} : result.module_name;
            ESP_LOGE(tag, "failed to register module '%.*s' (status=%u)",
                     static_cast<int>(module_name.size()), module_name.data(),
                     static_cast<unsigned>(result.status));
        }
        return static_cast<bool>(result);
    };

    if (!register_module(app_modules::create_board_module()) ||
        !register_module(app_modules::create_motion_module()) ||
        !register_module(app_modules::create_wave_module()) ||
        !register_module(app_modules::create_connectivity_module()) ||
        !register_module(app_modules::create_peripheral_module()) ||
        !register_module(app_modules::create_ui_module(button_events)) ||
        !register_module(app_modules::create_button_module(button_events))) {
        return;
    }

    const app::LifecycleResult initialized = manager.initialize_all();
    if (!initialized) {
        const std::string_view module_name =
            initialized.module_name.empty() ? std::string_view{"<unknown>"}
                                            : initialized.module_name;
        ESP_LOGE(tag, "application initialization failed at '%.*s' (status=%u)",
                 static_cast<int>(module_name.size()), module_name.data(),
                 static_cast<unsigned>(initialized.status));
        if (manager.has_cleanup_failure()) {
            ESP_LOGE(tag, "cleanup is incomplete; retaining module storage for task safety");
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        return;
    }
    ESP_LOGI(tag, "M5-StickS3 Motion UI started");

    while (true) {
        button_events.dispatch_pending();
        manager.process_all();
        uint32_t sleep_seconds = 0;
        if (sleep_service::take_request(sleep_seconds)) {
            sleep_service::enter_deep_sleep(manager, sleep_seconds);
            if (!manager.is_running()) {
                // A failed teardown or power transaction cannot leave a half
                // stopped application running. Boot restores rails and pads.
                ESP_LOGW(tag, "restarting after unsuccessful sleep preparation");
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
