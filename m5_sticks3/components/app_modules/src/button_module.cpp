#include "app_modules.hpp"

#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "bsp_board.hpp"
#include "button_debouncer.hpp"
#include "button_event_bus.hpp"
#include "driver/gpio.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr char kTag[] = "buttons";
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(10);
constexpr TickType_t kDebounceInterval = pdMS_TO_TICKS(30);

class ButtonTask final : public AppTask {
public:
    explicit ButtonTask(app::ButtonEventBus& event_bus)
        : AppTask("buttons", 3072, tskIDLE_PRIORITY + 2, 1),
          event_bus_(event_bus),
          key1_(app::ButtonId::key1, kDebounceInterval),
          key2_(app::ButtonId::key2, kDebounceInterval)
    {
    }

private:
    void update_button(gpio_num_t pin, app::ButtonDebouncer& button)
    {
        const bool pressed = gpio_get_level(pin) == 0;
        if (const auto event = button.update(pressed, xTaskGetTickCount())) {
            event_bus_.publish(*event);
        }
    }

    void main() override
    {
        while (!should_exit()) {
            update_button(bsp::kKey1, key1_);
            update_button(bsp::kKey2, key2_);
            vTaskDelay(kPollInterval);
        }
    }

    app::ButtonEventBus& event_bus_;
    app::ButtonDebouncer key1_;
    app::ButtonDebouncer key2_;
};

class ButtonModule final : public AppModule {
public:
    explicit ButtonModule(app::ButtonEventBus& event_bus) : event_bus_(event_bus) {}

    std::string_view name() const override { return "buttons"; }

private:
    bool on_initialize() override
    {
        if (!bsp::Board::instance().initialized()) {
            ESP_LOGE(kTag, "board must be initialized before the button module");
            return false;
        }

        event_bus_.clear();
        task_ = std::make_unique<ButtonTask>(event_bus_);
        if (!task_->start()) {
            task_.reset();
            return false;
        }

        ESP_LOGI(kTag, "button scanner ready (poll=10ms debounce=30ms)");
        return true;
    }

    bool on_deinitialize() override
    {
        if (task_ && !task_->stop()) {
            ESP_LOGE(kTag, "button task did not stop before timeout");
            return false;
        }

        task_.reset();
        event_bus_.clear();
        return true;
    }

    app::ButtonEventBus& event_bus_;
    std::unique_ptr<ButtonTask> task_;
};

} // namespace

std::unique_ptr<AppModule> create_button_module(app::ButtonEventBus& event_bus)
{
    return std::make_unique<ButtonModule>(event_bus);
}

} // namespace app_modules
