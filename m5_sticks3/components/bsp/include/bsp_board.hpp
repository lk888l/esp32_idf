#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace bsp {

constexpr i2c_port_t kInternalI2cPort = I2C_NUM_1;
constexpr gpio_num_t kInternalI2cSda = GPIO_NUM_47;
constexpr gpio_num_t kInternalI2cScl = GPIO_NUM_48;
constexpr uint32_t kInternalI2cClockHz = 400000;
constexpr uint8_t kBmi270Address = 0x68;
constexpr uint8_t kM5Pm1Address = 0x6E;

constexpr gpio_num_t kKey1 = GPIO_NUM_11;
constexpr gpio_num_t kKey2 = GPIO_NUM_12;

constexpr gpio_num_t kLcdMosi = GPIO_NUM_39;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_40;
constexpr gpio_num_t kLcdCs = GPIO_NUM_41;
constexpr gpio_num_t kLcdDc = GPIO_NUM_45;
constexpr gpio_num_t kLcdReset = GPIO_NUM_21;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_38;

class Board {
public:
    static Board& instance();

    esp_err_t initialize();
    esp_err_t deinitialize();
    esp_err_t enable_display_power();
    esp_err_t enable_5v_output();

    bool initialized() const { return initialized_; }
    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }

private:
    Board() = default;
    esp_err_t update_pm1_register(uint8_t reg, uint8_t set_mask, uint8_t clear_mask);
    esp_err_t write_pm1_register(uint8_t reg, uint8_t value);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t pm1_ = nullptr;
    bool initialized_ = false;
};

} // namespace bsp

