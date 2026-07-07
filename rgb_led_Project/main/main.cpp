#include "app_manager.hpp"
#include "app_module.hpp"
#include "app_task.hpp"
#include "esp_rmt_ws2812b_strip.hpp"
#include "logger.hpp"
#include "ws2812b_strip.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr int kLedGpio = 27;
constexpr std::size_t kLedCount = 16;
constexpr uint8_t kBrightness = 64;
constexpr uint8_t kHueStep = 2;
constexpr uint32_t kFrameDelayMs = 30;
constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;

hardware::Rgb wheel(uint8_t position)
{
    if (position < 85) {
        return {
            static_cast<uint8_t>(255 - position * 3),
            static_cast<uint8_t>(position * 3),
            0,
        };
    }

    if (position < 170) {
        position = static_cast<uint8_t>(position - 85);
        return {
            0,
            static_cast<uint8_t>(255 - position * 3),
            static_cast<uint8_t>(position * 3),
        };
    }

    position = static_cast<uint8_t>(position - 170);
    return {
        static_cast<uint8_t>(position * 3),
        0,
        static_cast<uint8_t>(255 - position * 3),
    };
}

class RainbowTask final : public AppTask {
public:
    explicit RainbowTask(hardware::IWs2812bStrip& strip)
        : AppTask("ws2812b_rainbow", kTaskStackSize, kTaskPriority)
        , strip_(strip)
    {
    }

private:
    void main() override
    {
        uint8_t hue = 0;

        while (!shouldExit()) {
            for (std::size_t i = 0; i < strip_.size(); ++i) {
                const uint8_t pixel_hue = static_cast<uint8_t>(
                    hue + (i * 256U / strip_.size()));
                strip_.setPixel(i, wheel(pixel_hue));
            }

            strip_.show();
            hue = static_cast<uint8_t>(hue + kHueStep);
            vTaskDelay(pdMS_TO_TICKS(kFrameDelayMs));
        }

        strip_.clear();
        strip_.show();
    }

    hardware::IWs2812bStrip& strip_;
};

class RainbowModule final : public AppModule {
public:
    RainbowModule()
        : strip_({
            .data_pin = kLedGpio,
            .led_count = kLedCount,
            .brightness = kBrightness,
        })
        , task_(strip_)
    {
    }

    bool initialize() override
    {
        if (initialized_) {
            return true;
        }

        if (!strip_.initialize()) {
            log_.error("failed to initialize WS2812B strip");
            return false;
        }

        strip_.clear();
        strip_.show();

        if (!task_.start()) {
            strip_.deinitialize();
            log_.error("failed to start WS2812B rainbow task");
            return false;
        }

        initialized_ = true;
        log_.info("rainbow demo started on GPIO {}, LEDs={}", kLedGpio, kLedCount);
        return true;
    }

    bool deinitialize() override
    {
        if (!initialized_) {
            return true;
        }

        const bool stopped = task_.stop();
        strip_.clear();
        strip_.show();
        strip_.deinitialize();

        initialized_ = !stopped;
        return stopped;
    }

    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "ws2812b_rainbow_demo"; }

private:
    hardware::EspRmtWs2812bStrip strip_;
    RainbowTask task_;
    esp_template::Logger log_{"ws2812b_demo"};
    bool initialized_ = false;
};

} // namespace

extern "C" void app_main(void)
{
    esp_template::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(std::make_unique<RainbowModule>())) {
        log.error("failed to register WS2812B demo module");
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

    log.info("RGB LED application started");

    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
