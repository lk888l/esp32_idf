#include "app_manager.hpp"
#include "app_modules.hpp"
#include "esp_log.h"

#include <memory>
#include <string_view>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    constexpr char kTag[] = "main";
    app::AppManager manager;

    const auto register_module = [&](std::unique_ptr<AppModule> module) {
        const app::RegistrationResult result = manager.register_module(std::move(module));
        if (!result) {
            const std::string_view module_name =
                result.module_name.empty() ? std::string_view{"<unnamed>"} : result.module_name;
            ESP_LOGE(kTag, "failed to register module '%.*s' (status=%u)",
                     static_cast<int>(module_name.size()),
                     module_name.data(),
                     static_cast<unsigned>(result.status));
        }
        return static_cast<bool>(result);
    };

    if (!register_module(app_modules::create_gpio_blink_module())) {
        return;
    }

    const app::LifecycleResult initialized = manager.initialize_all();
    if (!initialized) {
        const std::string_view module_name =
            initialized.module_name.empty() ? std::string_view{"<unknown>"}
                                            : initialized.module_name;
        ESP_LOGE(kTag, "application initialization failed at '%.*s' (status=%u)",
                 static_cast<int>(module_name.size()),
                 module_name.data(),
                 static_cast<unsigned>(initialized.status));
        if (manager.has_cleanup_failure()) {
            ESP_LOGE(kTag, "cleanup is incomplete; retaining module storage for task safety");
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        return;
    }

    ESP_LOGI(kTag, "application started");

    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
