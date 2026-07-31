#include "app_manager.hpp"
#include "app_module.hpp"
#include "app_task.hpp"
#include "board_config.hpp"
#include "esp_i2c_bus.hpp"
#include "logger.hpp"
#include "oled_canvas.hpp"
#include "oled_display.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr std::uint32_t kDisplayTaskStackSize = 4096;
constexpr UBaseType_t kDisplayTaskPriority = tskIDLE_PRIORITY + 1;
constexpr std::uint32_t kRefreshPeriodMs = 1000;
constexpr std::uint32_t kStopCheckPeriodMs = 100;
constexpr std::uint64_t kPageDurationSeconds = 3;

void delayMs(std::uint32_t milliseconds)
{
    if (milliseconds == 0) {
        return;
    }

    const TickType_t converted = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(converted == 0 ? 1 : converted);
}

void delayUs(std::uint32_t microseconds)
{
    if (microseconds > 0) {
        esp_rom_delay_us(microseconds);
    }
}

class OledDemoTask final : public AppTask {
public:
    explicit OledDemoTask(display::OledDisplay& display)
        : AppTask("oled_demo", kDisplayTaskStackSize, kDisplayTaskPriority)
        , display_(display)
        , canvas_(display.nativeHandle())
        , boot_time_us_(esp_timer_get_time())
    {
    }

private:
    enum class Page : std::uint8_t {
        status,
        graphics,
        grayscale,
    };

    std::uint64_t uptimeSeconds() const
    {
        const std::int64_t elapsed_us = esp_timer_get_time() - boot_time_us_;
        return elapsed_us > 0 ? static_cast<std::uint64_t>(elapsed_us / 1000000) : 0;
    }

    void drawStatus(std::uint64_t uptime_seconds)
    {
        char text[32] = {};

        canvas_.clear();
        canvas_.setFont(display::Font::small);
        canvas_.drawText(0, 7, "ESP32 OLED");
        canvas_.drawLine(0, 9, 127, 9, 128);

        std::snprintf(text,
                      sizeof(text),
                      "Uptime: %llu s",
                      static_cast<unsigned long long>(uptime_seconds));
        canvas_.drawText(0, 20, text);

        std::snprintf(text,
                      sizeof(text),
                      "Free heap: %u",
                      static_cast<unsigned int>(esp_get_free_heap_size()));
        canvas_.drawText(0, 31, text);

        std::snprintf(text,
                      sizeof(text),
                      "Frames: %llu",
                      static_cast<unsigned long long>(frame_count_));
        canvas_.drawText(0, 42, text);

        const float minute_percent =
            static_cast<float>(uptime_seconds % 60U) * (100.0F / 59.0F);
        canvas_.drawProgressBar(0, 47, 128, 8, minute_percent, 176);
        canvas_.drawText(0, 63, "SSD1306  I2C 400kHz", 160);
    }

    void drawGraphics()
    {
        canvas_.clear();
        canvas_.setFont(display::Font::small);
        canvas_.drawText(0, 7, "GRAPHICS");
        canvas_.drawRect(0, 10, 34, 22);
        canvas_.fillRect(4, 14, 26, 14, 96);
        canvas_.drawCircle(48, 21, 11);
        canvas_.fillCircle(72, 21, 10, 144);
        canvas_.drawTriangle(88, 31, 104, 11, 122, 31, 224);
        canvas_.drawArc(17, 49, 13, 200, 520, 192);
        canvas_.drawProgressBar(37, 42, 90, 9, 68.0F, 160);
        canvas_.setFont(display::Font::medium);
        canvas_.drawText(39, 63, "Line Circle Arc");
    }

    void drawGrayscale()
    {
        canvas_.clear();
        canvas_.setFont(display::Font::small);
        canvas_.drawText(0, 7, "BAYER GRAYSCALE");

        constexpr std::uint8_t levels[] = {0, 42, 85, 128, 170, 213, 255};
        for (std::size_t index = 0; index < std::size(levels); ++index) {
            const int x = static_cast<int>(index) * 18 + 1;
            canvas_.fillRect(x, 12, 16, 32, levels[index]);
            canvas_.drawRect(x, 12, 16, 32, 255);
        }

        canvas_.drawText(0, 54, "0");
        canvas_.drawText(106, 54, "255");
        canvas_.drawText(0, 63, "Stable spatial dither", 144);
    }

    void render(Page page, std::uint64_t uptime_seconds)
    {
        switch (page) {
        case Page::status:
            drawStatus(uptime_seconds);
            break;
        case Page::graphics:
            drawGraphics();
            break;
        case Page::grayscale:
            drawGrayscale();
            break;
        }
    }

    void waitForNextFrame()
    {
        constexpr std::uint32_t checks = kRefreshPeriodMs / kStopCheckPeriodMs;
        for (std::uint32_t index = 0; index < checks && !shouldExit(); ++index) {
            vTaskDelay(pdMS_TO_TICKS(kStopCheckPeriodMs));
        }
    }

    void main() override
    {
        ium::Logger log("oled_demo");
        log.info("display task started");

        while (!shouldExit()) {
            const std::uint64_t uptime_seconds = uptimeSeconds();
            const auto page = static_cast<Page>(
                (uptime_seconds / kPageDurationSeconds) % 3U);

            render(page, uptime_seconds);
            const display::DisplayStatus status = display_.present();
            if (status != display::DisplayStatus::ok) {
                log.error("display refresh failed: {}", display::toString(status));
                break;
            }

            ++frame_count_;
            waitForNextFrame();
        }

        log.info("display task stopped");
    }

    display::OledDisplay& display_;
    display::OledCanvas canvas_;
    std::int64_t boot_time_us_ = 0;
    std::uint64_t frame_count_ = 0;
};

class OledModule final : public AppModule {
public:
    OledModule()
        : bus_({
              .port = bsp::kOledI2cPort,
              .sda = bsp::kOledI2cSda,
              .scl = bsp::kOledI2cScl,
              .enable_internal_pullups = true,
          })
    {
    }

    bool initialize() override
    {
        if (initialized_) {
            return true;
        }

        const bsp::I2CStatus bus_status = bus_.init();
        if (bus_status != bsp::I2CStatus::ok) {
            log_.error("I2C bus initialization failed: {}", bsp::toString(bus_status));
            return false;
        }

        bsp::I2CDeviceResult device_result =
            bus_.createDevice(bsp::kOledI2cAddress, bsp::kOledI2cClockHz);
        if (!device_result) {
            log_.error("OLED device creation failed: {}", bsp::toString(device_result.status));
            bus_.deinit();
            return false;
        }
        device_ = std::move(device_result.device);

        display::OledDisplayConfig display_config = {
            .rotation = display::Rotation::deg0,
            .contrast = 0xCF,
            .delay_ms = delayMs,
            .delay_us = delayUs,
        };
        display_ = std::make_unique<display::OledDisplay>(*device_, display_config);

        const display::DisplayStatus display_status = display_->initialize();
        if (display_status != display::DisplayStatus::ok) {
            log_.error("OLED initialization failed: {}", display::toString(display_status));
            releaseTransport();
            return false;
        }

        task_ = std::make_unique<OledDemoTask>(*display_);
        if (!task_->start()) {
            log_.error("failed to start OLED display task");
            display_->setPowerSave(true);
            task_.reset();
            releaseTransport();
            return false;
        }

        initialized_ = true;
        log_.info("initialized: SDA={}, SCL={}, address=0x{:02X}, clock={}Hz",
                  static_cast<int>(bsp::kOledI2cSda),
                  static_cast<int>(bsp::kOledI2cScl),
                  bsp::kOledI2cAddress,
                  bsp::kOledI2cClockHz);
        return true;
    }

    bool deinitialize() override
    {
        if (!initialized_) {
            return true;
        }

        if (task_ != nullptr && !task_->stop()) {
            log_.error("OLED display task did not stop before timeout");
            return false;
        }
        task_.reset();

        bool ok = true;
        if (display_ != nullptr) {
            const display::DisplayStatus status = display_->setPowerSave(true);
            if (status != display::DisplayStatus::ok) {
                log_.error("failed to enter OLED power-save mode: {}",
                           display::toString(status));
                ok = false;
            }
        }

        display_.reset();
        device_.reset();

        const bsp::I2CStatus bus_status = bus_.deinit();
        if (bus_status != bsp::I2CStatus::ok) {
            log_.error("I2C bus deinitialization failed: {}", bsp::toString(bus_status));
            ok = false;
        }

        initialized_ = false;
        return ok;
    }

    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "ssd1306_oled"; }

private:
    void releaseTransport()
    {
        display_.reset();
        device_.reset();
        const bsp::I2CStatus status = bus_.deinit();
        if (status != bsp::I2CStatus::ok) {
            log_.error("I2C cleanup failed: {}", bsp::toString(status));
        }
    }

    ium::Logger log_{"ssd1306_oled"};
    bsp::EspI2CBus bus_;
    std::unique_ptr<bsp::I2CDevice> device_;
    std::unique_ptr<display::OledDisplay> display_;
    std::unique_ptr<OledDemoTask> task_;
    bool initialized_ = false;
};

} // namespace

extern "C" void app_main(void)
{
    ium::Logger log("main");
    auto& manager = app::AppManager::get_instance();

    if (!manager.register_module(std::make_unique<OledModule>())) {
        log.error("failed to register SSD1306 OLED module");
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

    log.info("SSD1306 OLED application started");

    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
