#include "foc_demo_module.hpp"

#include <algorithm>
#include <cinttypes>

#include "board_config.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr uint32_t kWorkerStackSizeBytes = 4096;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 3;

} // namespace

FocWorkerTask::FocWorkerTask(motor_control::StepperFoc& foc,
                             hardware::Mt6816& encoder) noexcept
    : AppTask("foc_worker", kWorkerStackSizeBytes, kWorkerPriority, 0)
    , foc_(foc)
    , encoder_(encoder)
{
}

void FocWorkerTask::run()
{
    constexpr uint32_t task_period_ms = board::kOpenLoopDemoEnabled
        ? board::kDemoUpdatePeriodMs
        : board::kEncoderMonitorPeriodMs;
    const TickType_t task_period_ticks =
        std::max<TickType_t>(1, pdMS_TO_TICKS(task_period_ms));

    TickType_t last_wake_tick = xTaskGetTickCount();
    uint16_t electrical_index = 0;
    const TickType_t encoder_period_ticks =
        std::max<TickType_t>(1, pdMS_TO_TICKS(board::kEncoderMonitorPeriodMs));
    TickType_t encoder_elapsed_ticks = encoder_period_ticks;

    if constexpr (board::kOpenLoopDemoEnabled) {
        log_.warn("open-loop TB6612 demo ENABLED: amplitude=%u/1000, period=%" PRIu32 " ms",
                  static_cast<unsigned>(board::kDemoAmplitudePermille),
                  board::kDemoUpdatePeriodMs);
    } else {
        log_.info("motor output disabled; monitoring MT6816 every %" PRIu32 " ms",
                  board::kEncoderMonitorPeriodMs);
    }

    while (!shouldExit()) {
        if constexpr (board::kOpenLoopDemoEnabled) {
            if (!foc_.command(electrical_index, board::kDemoAmplitudePermille)) {
                log_.error("failed to update bridge PWM; disabling motor output");
                foc_.disable();
            }
            electrical_index = static_cast<uint16_t>(
                (electrical_index + board::kDemoElectricalStepPerTick) & 0x03FFU);
        }

        if (encoder_elapsed_ticks >= encoder_period_ticks) {
            encoder_elapsed_ticks = 0;
            const hardware::Mt6816Reading reading = encoder_.read();
            if (!reading.valid()) {
                log_.warn("MT6816 read failed: status=%u, attempts=%u",
                          static_cast<unsigned>(reading.status),
                          static_cast<unsigned>(reading.attempts));
            } else if (reading.no_magnet) {
                log_.warn("MT6816 reports no magnet, raw_angle=%u",
                          static_cast<unsigned>(reading.angle));
            } else {
                log_.info("MT6816 raw_angle=%u/%u",
                          static_cast<unsigned>(reading.angle),
                          static_cast<unsigned>(hardware::Mt6816::kResolution));
            }
        }

        encoder_elapsed_ticks += task_period_ticks;
        vTaskDelayUntil(&last_wake_tick, task_period_ticks);
    }
}

void FocWorkerTask::cleanup()
{
    foc_.disable();
}

FocDemoModule::FocDemoModule() noexcept
    : bridge_(makeBridgeConfig())
    , encoder_spi_(makeEncoderSpiConfig())
    , encoder_(encoder_spi_)
    , foc_(bridge_)
    , worker_(foc_, encoder_)
{
}

bool FocDemoModule::initialize()
{
    if (initialized_) {
        return true;
    }

    if (!bridge_.initialize()) {
        log_.error("failed to initialize TB6612 BSP");
        return false;
    }
    if (!encoder_spi_.initialize()) {
        log_.error("failed to initialize encoder SPI BSP");
        bridge_.deinitialize();
        return false;
    }
    if (!worker_.start()) {
        log_.error("failed to start FOC worker task");
        encoder_spi_.deinitialize();
        bridge_.deinitialize();
        return false;
    }

    initialized_ = true;
    log_.info("initialized with safe motor default=%s",
              board::kOpenLoopDemoEnabled ? "enabled" : "disabled");
    return true;
}

bool FocDemoModule::deinitialize()
{
    if (!initialized_) {
        return true;
    }
    if (!worker_.stop()) {
        log_.error("worker task did not stop; keeping BSP resources alive");
        return false;
    }

    foc_.disable();
    encoder_spi_.deinitialize();
    bridge_.deinitialize();
    initialized_ = false;
    return true;
}

bsp::Tb6612BridgeConfig FocDemoModule::makeBridgeConfig() noexcept
{
    bsp::Tb6612BridgeConfig config{};
    config.phase_a_pwm_gpio = board::kTb6612PwmAGpio;
    config.phase_a_in1_gpio = board::kTb6612Ain1Gpio;
    config.phase_a_in2_gpio = board::kTb6612Ain2Gpio;
    config.phase_b_pwm_gpio = board::kTb6612PwmBGpio;
    config.phase_b_in1_gpio = board::kTb6612Bin1Gpio;
    config.phase_b_in2_gpio = board::kTb6612Bin2Gpio;
    config.standby_gpio = board::kTb6612StandbyGpio;
    config.pwm_frequency_hz = board::kPwmFrequencyHz;
    return config;
}

bsp::SpiRegisterDeviceConfig FocDemoModule::makeEncoderSpiConfig() noexcept
{
    bsp::SpiRegisterDeviceConfig config{};
    config.host = SPI2_HOST;
    config.sclk_gpio = board::kEncoderSclkGpio;
    config.miso_gpio = board::kEncoderMisoGpio;
    config.mosi_gpio = board::kEncoderMosiGpio;
    config.chip_select_gpio = board::kEncoderChipSelectGpio;
    config.clock_hz = board::kEncoderSpiClockHz;
    config.mode = board::kEncoderSpiMode;
    return config;
}

} // namespace app_modules
