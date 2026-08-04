#include "bsp_board.hpp"

#include <array>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace bsp {
namespace {

constexpr char kTag[] = "board";

} // namespace

Board& Board::instance()
{
    static Board board;
    return board;
}

esp_err_t Board::initialize()
{
    if (initialized_) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = kInternalI2cPort,
        .sda_io_num = kInternalI2cSda,
        .scl_io_num = kInternalI2cScl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag,
                        "failed to create internal I2C bus");

    const i2c_device_config_t pm1_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kM5Pm1Address,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };
    esp_err_t result = i2c_master_bus_add_device(i2c_bus_, &pm1_config, &pm1_);
    if (result != ESP_OK) {
        const esp_err_t cleanup_result = deinitialize();
        if (cleanup_result != ESP_OK) {
            ESP_LOGE(kTag, "failed to roll back I2C bus: %s",
                     esp_err_to_name(cleanup_result));
        }
        return result;
    }

    gpio_config_t keys = {};
    keys.pin_bit_mask = (1ULL << kKey1) | (1ULL << kKey2);
    keys.mode = GPIO_MODE_INPUT;
    keys.pull_up_en = GPIO_PULLUP_ENABLE;
    keys.pull_down_en = GPIO_PULLDOWN_DISABLE;
    keys.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&keys);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure keys: %s", esp_err_to_name(result));
        deinitialize();
        return result;
    }

    initialized_ = true;
    ESP_LOGI(kTag, "StickS3 BSP ready (I2C SDA=%d SCL=%d)", kInternalI2cSda, kInternalI2cScl);
    return ESP_OK;
}

esp_err_t Board::enable_display_power()
{
    ESP_RETURN_ON_FALSE(initialized_ && pm1_, ESP_ERR_INVALID_STATE, kTag,
                        "board is not initialized");

    // M5PM1 GPIO2 controls the L3B rail used by the LCD and backlight.
    ESP_RETURN_ON_ERROR(update_pm1_register(0x16, 0x00, 1U << 2), kTag,
                        "failed to select PM1 GPIO2 function");
    ESP_RETURN_ON_ERROR(update_pm1_register(0x10, 1U << 2, 0x00), kTag,
                        "failed to set PM1 GPIO2 output mode");
    ESP_RETURN_ON_ERROR(update_pm1_register(0x13, 0x00, 1U << 2), kTag,
                        "failed to set PM1 GPIO2 push-pull mode");
    ESP_RETURN_ON_ERROR(update_pm1_register(0x11, 1U << 2, 0x00), kTag,
                        "failed to enable LCD L3B rail");
    ESP_RETURN_ON_ERROR(write_pm1_register(0x09, 0x00), kTag,
                        "failed to disable PM1 I2C idle sleep");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t Board::deinitialize()
{
    if (!initialized_ && !pm1_ && !i2c_bus_) {
        return ESP_OK;
    }
    esp_err_t result = ESP_OK;
    if (pm1_) {
        result = i2c_master_bus_rm_device(pm1_);
        if (result != ESP_OK) {
            initialized_ = false;
            return result;
        }
        pm1_ = nullptr;
    }
    if (i2c_bus_) {
        result = i2c_del_master_bus(i2c_bus_);
        if (result != ESP_OK) {
            initialized_ = false;
            return result;
        }
        i2c_bus_ = nullptr;
    }
    initialized_ = false;
    return result;
}

esp_err_t Board::update_pm1_register(uint8_t reg, uint8_t set_mask, uint8_t clear_mask)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(pm1_, &reg, 1, &value, 1, 100), kTag,
                        "PM1 register 0x%02x read failed", reg);
    value = static_cast<uint8_t>((value | set_mask) & ~clear_mask);
    return write_pm1_register(reg, value);
}

esp_err_t Board::write_pm1_register(uint8_t reg, uint8_t value)
{
    const std::array<uint8_t, 2> data{reg, value};
    return i2c_master_transmit(pm1_, data.data(), data.size(), 100);
}

} // namespace bsp
