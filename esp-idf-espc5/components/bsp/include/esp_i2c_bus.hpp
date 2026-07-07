#pragma once

#include <cstdint>

#include "bsp_i2c.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace bsp {

struct EspI2CBusConfig {
    i2c_port_t port;
    gpio_num_t sda;
    gpio_num_t scl;
    bool enable_internal_pullups = true;
};

class EspI2CBus final : public I2CBus {
public:
    explicit EspI2CBus(const EspI2CBusConfig& config);
    ~EspI2CBus() override;

    EspI2CBus(const EspI2CBus&) = delete;
    EspI2CBus& operator=(const EspI2CBus&) = delete;

    I2CStatus init() override;
    I2CStatus deinit() override;
    I2CDeviceResult createDevice(uint8_t address, uint32_t clock_hz) override;

    bool isInitialized() const override { return bus_ != nullptr; }

private:
    EspI2CBusConfig config_;
    i2c_master_bus_handle_t bus_ = nullptr;
};

} // namespace bsp
