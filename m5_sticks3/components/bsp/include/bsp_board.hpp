#pragma once

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace bsp {

constexpr i2c_port_t kInternalI2cPort = I2C_NUM_1;
constexpr gpio_num_t kInternalI2cSda = GPIO_NUM_47;
constexpr gpio_num_t kInternalI2cScl = GPIO_NUM_48;
constexpr uint32_t kInternalI2cClockHz = 400000;
constexpr uint8_t kBmi270Address = 0x68;
constexpr uint8_t kM5Pm1Address = 0x6E;
constexpr uint8_t kEs8311Address = 0x18;

constexpr gpio_num_t kKey1 = GPIO_NUM_11;
constexpr gpio_num_t kKey2 = GPIO_NUM_12;

constexpr gpio_num_t kLcdMosi = GPIO_NUM_39;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_40;
constexpr gpio_num_t kLcdCs = GPIO_NUM_41;
constexpr gpio_num_t kLcdDc = GPIO_NUM_45;
constexpr gpio_num_t kLcdReset = GPIO_NUM_21;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_38;

// Names are from the ESP32's perspective: DOUT -> ES8311 DIN, DIN <- ES8311 DOUT.
constexpr gpio_num_t kAudioMclk = GPIO_NUM_18;
constexpr gpio_num_t kAudioBclk = GPIO_NUM_17;
constexpr gpio_num_t kAudioWs = GPIO_NUM_15;
constexpr gpio_num_t kAudioDout = GPIO_NUM_14;
constexpr gpio_num_t kAudioDin = GPIO_NUM_16;
constexpr gpio_num_t kIrTx = GPIO_NUM_46;
constexpr gpio_num_t kIrRx = GPIO_NUM_42;

struct PowerStatus {
    uint16_t battery_mv = 0;
    uint16_t input_mv = 0;
    uint16_t external_mv = 0;
    bool boost_enabled = false;

    bool externally_powered() const
    {
        return input_mv >= 4000 || (!boost_enabled && external_mv >= 4000);
    }
};

// Initialize before consumers; deinitialize only after all consumers have stopped.
// Power methods serialize entire PM1 read/modify/write transactions. They may block:
// call them from service tasks, never from LVGL callbacks or an interrupt.
class Board {
public:
    static Board& instance();

    esp_err_t initialize();
    esp_err_t deinitialize();
    esp_err_t enable_display_power();
    // Explicit persistent request; never changes GPIO0 (charging input) or GPIO1.
    esp_err_t enable_5v_output();
    esp_err_t read_power_status(PowerStatus& status);

    // AW8737 and IR RX are mutually exclusive even when called from different tasks.
    // A conflicting acquisition returns ESP_ERR_INVALID_STATE, without preemption.
    esp_err_t set_speaker_enabled(bool enabled);
    esp_err_t acquire_ir_receiver();
    esp_err_t release_ir_receiver();
    bool ir_receiver_active() const { return ir_receiver_active_.load(); }

    // Balanced leases preserve pre-existing boost/external-input state. A failed
    // release retains its lease so the caller can retry cleanup.
    esp_err_t acquire_ir_power();
    esp_err_t release_ir_power();

    bool initialized() const { return initialized_.load(); }
    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }

private:
    Board() = default;
    esp_err_t update_pm1_register(uint8_t reg, uint8_t set_mask, uint8_t clear_mask);
    esp_err_t write_pm1_register(uint8_t reg, uint8_t value);
    esp_err_t read_pm1_register(uint8_t reg, uint8_t& value);
    esp_err_t read_power_status_locked(PowerStatus& status);
    esp_err_t ensure_5v_locked(bool& enabled_by_us);
    esp_err_t set_speaker_locked(bool enabled);
    esp_err_t deinitialize_locked();

    StaticSemaphore_t power_mutex_storage_{};
    SemaphoreHandle_t power_mutex_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t pm1_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> ir_receiver_active_{false};
    bool speaker_enabled_ = false;
    bool boost_owned_ = false;
    bool output_pinned_ = false;
    uint32_t ir_power_users_ = 0;
};

} // namespace bsp
