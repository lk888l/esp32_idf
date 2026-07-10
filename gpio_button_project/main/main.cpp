#include "app_manager.hpp"
#include "app_module.hpp"
#include "gpio_button_service.hpp"
#include "logger.hpp"

#include <memory>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr gpio_num_t kButtonGpio = GPIO_NUM_0;

class ButtonModule final : public AppModule {
public:
    bool initialize() override
    {
        if (initialized_) {
            return true;
        }

        const button::ButtonConfig config{
            .gpio = kButtonGpio,
            .active_low = true,
            .pull_mode = GPIO_PULLUP_ONLY,
            .debounce_ms = 20,
            .long_press_ms = 800,
        };
        if (!buttons_.add_button(config) || !buttons_.start()) {
            return false;
        }

        initialized_ = true;
        log_.info("initialized: GPIO={}, active-low, debounce=20ms, long-press=800ms",
                  static_cast<int>(kButtonGpio));
        return true;
    }

    bool deinitialize() override
    {
        if (!initialized_) {
            return true;
        }

        const bool stopped = buttons_.stop();
        initialized_ = !stopped;
        return stopped;
    }

    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "gpio_button"; }

    void process() override
    {
        button::ButtonEvent event{};
        while (buttons_.try_pop(event)) {
            log_.info("GPIO {} {} at {}ms",
                      static_cast<int>(event.gpio),
                      event.type == button::ButtonEventType::Click ? "click" : "long press",
                      event.timestamp_ms);
        }
    }

private:
    ium::Logger log_{"gpio_button"};
    button::GpioButtonService buttons_;
    bool initialized_ = false;
};

} // namespace

extern "C" void app_main(void)
{
    ium::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(std::make_unique<ButtonModule>())) {
        log.error("failed to register gpio button module");
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
