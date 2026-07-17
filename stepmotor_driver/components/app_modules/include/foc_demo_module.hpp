#pragma once

#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "esp_tb6612_bridge.hpp"
#include "esp_spi_register_device.hpp"
#include "logger.hpp"
#include "mt6816.hpp"
#include "stepper_foc.hpp"

namespace app_modules {

class FocWorkerTask final : public app::AppTask {
public:
    FocWorkerTask(motor_control::StepperFoc& foc, hardware::Mt6816& encoder) noexcept;

private:
    void run() override;
    void cleanup() override;

    motor_control::StepperFoc& foc_;
    hardware::Mt6816& encoder_;
    logger::Logger log_{"foc_worker"};
};

class FocDemoModule final : public app::AppModule {
public:
    FocDemoModule() noexcept;

    bool initialize() override;
    bool deinitialize() override;
    bool isInitialized() const noexcept override { return initialized_; }
    std::string_view name() const noexcept override { return "foc_demo"; }

private:
    static bsp::Tb6612BridgeConfig makeBridgeConfig() noexcept;
    static bsp::SpiRegisterDeviceConfig makeEncoderSpiConfig() noexcept;

    bsp::EspTb6612Bridge bridge_;
    bsp::EspSpiRegisterDevice encoder_spi_;
    hardware::Mt6816 encoder_;
    motor_control::StepperFoc foc_;
    FocWorkerTask worker_;
    logger::Logger log_{"foc_demo"};
    bool initialized_ = false;
};

} // namespace app_modules
