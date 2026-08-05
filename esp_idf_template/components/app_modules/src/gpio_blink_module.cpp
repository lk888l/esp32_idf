#include "app_modules.hpp"

#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr char kTag[] = "gpio_blink";
constexpr gpio_num_t kBlinkGpio = GPIO_NUM_8;
constexpr uint32_t kBlinkIntervalMs = 500;
constexpr uint32_t kBlinkTaskStackSize = 2048;
constexpr UBaseType_t kBlinkTaskPriority = tskIDLE_PRIORITY + 1;

bool configure_blink_gpio()
{
    esp_err_t result = gpio_reset_pin(kBlinkGpio);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "failed to reset GPIO %d: %s",
                 static_cast<int>(kBlinkGpio), esp_err_to_name(result));
        return false;
    }

    result = gpio_set_direction(kBlinkGpio, GPIO_MODE_OUTPUT);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure GPIO %d as output: %s",
                 static_cast<int>(kBlinkGpio), esp_err_to_name(result));
        return false;
    }

    result = gpio_set_level(kBlinkGpio, 0);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "failed to set GPIO %d low: %s",
                 static_cast<int>(kBlinkGpio), esp_err_to_name(result));
        return false;
    }
    return true;
}

class GpioBlinkTask final : public AppTask {
public:
    GpioBlinkTask()
        : AppTask("gpio_blink", kBlinkTaskStackSize, kBlinkTaskPriority)
    {
    }

private:
    void main() override
    {
        bool level = false;
        ESP_LOGI(kTag, "blink task started: GPIO=%d interval=%lums",
                 static_cast<int>(kBlinkGpio),
                 static_cast<unsigned long>(kBlinkIntervalMs));

        while (!should_exit()) {
            level = !level;
            const esp_err_t result = gpio_set_level(kBlinkGpio, level ? 1 : 0);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "failed to drive GPIO %d: %s",
                         static_cast<int>(kBlinkGpio), esp_err_to_name(result));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kBlinkIntervalMs));
        }

        gpio_set_level(kBlinkGpio, 0);
        ESP_LOGI(kTag, "blink task stopped");
    }
};

class GpioBlinkModule final : public AppModule {
public:
    std::string_view name() const override { return "gpio_blink"; }

private:
    bool on_initialize() override
    {
        if (!configure_blink_gpio()) {
            return false;
        }
        if (!task_.start()) {
            ESP_LOGE(kTag, "failed to start blink task");
            return false;
        }

        ESP_LOGI(kTag, "module initialized");
        return true;
    }

    bool on_deinitialize() override
    {
        if (!task_.stop()) {
            ESP_LOGE(kTag, "blink task did not stop before timeout");
            return false;
        }

        const esp_err_t result = gpio_set_level(kBlinkGpio, 0);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "failed to restore GPIO %d low: %s",
                     static_cast<int>(kBlinkGpio), esp_err_to_name(result));
            return false;
        }
        return true;
    }

    GpioBlinkTask task_;
};

} // namespace

std::unique_ptr<AppModule> create_gpio_blink_module()
{
    return std::make_unique<GpioBlinkModule>();
}

} // namespace app_modules
