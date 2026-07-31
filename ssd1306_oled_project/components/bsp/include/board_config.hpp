#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

namespace bsp {

inline constexpr i2c_port_t kOledI2cPort = I2C_NUM_0;
inline constexpr gpio_num_t kOledI2cSda = GPIO_NUM_8;
inline constexpr gpio_num_t kOledI2cScl = GPIO_NUM_9;
inline constexpr std::uint32_t kOledI2cClockHz = 400000;
inline constexpr std::uint8_t kOledI2cAddress = 0x3C;
} // namespace bsp
