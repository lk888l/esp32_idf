#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace bsp {

constexpr i2c_port_t kImuI2cPort = I2C_NUM_0;
constexpr gpio_num_t kImuI2cSda = GPIO_NUM_8;
constexpr gpio_num_t kImuI2cScl = GPIO_NUM_9;
constexpr uint32_t kImuI2cClockHz = 400000;
constexpr uint8_t kIcm42688Address = 0x68;

} // namespace bsp
