#pragma once

#include <cstdint>

#include "driver/spi_master.h"
#include "hal/gpio_types.h"

#include "spi_register_device.hpp"

namespace bsp {

struct SpiRegisterDeviceConfig {
    spi_host_device_t host = SPI2_HOST;
    gpio_num_t sclk_gpio = GPIO_NUM_NC;
    gpio_num_t miso_gpio = GPIO_NUM_NC;
    gpio_num_t mosi_gpio = GPIO_NUM_NC;
    gpio_num_t chip_select_gpio = GPIO_NUM_NC;
    uint32_t clock_hz = 1'000'000;
    uint8_t mode = 0;
};

class EspSpiRegisterDevice final : public SpiRegisterDevice {
public:
    explicit EspSpiRegisterDevice(const SpiRegisterDeviceConfig& config) noexcept;
    ~EspSpiRegisterDevice() override;

    bool initialize() noexcept;
    void deinitialize() noexcept;
    bool isInitialized() const noexcept { return initialized_; }

    bool readRegister(uint8_t register_address, uint8_t& value) noexcept override;

private:
    SpiRegisterDeviceConfig config_;
    spi_device_handle_t device_ = nullptr;
    bool bus_initialized_ = false;
    bool initialized_ = false;
};

} // namespace bsp
