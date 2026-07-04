#include "app_manager.hpp"
#include "app_module.hpp"
#include "app_task.hpp"
#include "logger.hpp"

#include <memory>
#include <string_view>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr gpio_num_t kBlinkGpio = GPIO_NUM_25;
constexpr uint32_t kBlinkIntervalMs = 500;
constexpr uint32_t kBlinkTaskStackSize = 2048;
constexpr UBaseType_t kBlinkTaskPriority = tskIDLE_PRIORITY + 1;

class GpioBlinkTask final : public AppTask {
public:
    GpioBlinkTask()
        : AppTask("gpio_blink", kBlinkTaskStackSize, kBlinkTaskPriority)
    {
    }

private:
    void main() override
    {
        ium::Logger log("gpio_blink");
        bool level = false;

        log.info("blink task started on GPIO {}", static_cast<int>(kBlinkGpio));

        while (!shouldExit()) {
            gpio_set_level(kBlinkGpio, level ? 1 : 0);
            level = !level;
            vTaskDelay(pdMS_TO_TICKS(kBlinkIntervalMs));
        }

        gpio_set_level(kBlinkGpio, 0);
        log.info("blink task stopped");
    }
};

class GpioBlinkModule final : public AppModule {
public:
    bool initialize() override
    {
        if (initialized_) {
            return true;
        }

        gpio_reset_pin(kBlinkGpio);
        gpio_set_direction(kBlinkGpio, GPIO_MODE_OUTPUT);
        gpio_set_level(kBlinkGpio, 0);

        if (!task_.start()) {
            return false;
        }

        initialized_ = true;
        log_.info("initialized, GPIO={}, interval={}ms",
                  static_cast<int>(kBlinkGpio),
                  kBlinkIntervalMs);
        return true;
    }

    bool deinitialize() override
    {
        if (!initialized_) {
            return true;
        }

        const bool stopped = task_.stop();
        gpio_set_level(kBlinkGpio, 0);
        initialized_ = !stopped;
        return stopped;
    }

    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "gpio_blink"; }

private:
    ium::Logger log_{"gpio_blink"};
    GpioBlinkTask task_;
    bool initialized_ = false;
};

} // namespace

extern "C" void app_main(void)
{
    ium::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(std::make_unique<GpioBlinkModule>())) {
        log.error("failed to register gpio blink module");
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
