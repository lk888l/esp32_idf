#include "bsp_board.hpp"

#include <array>
#include <limits>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace bsp {
namespace {

constexpr char kTag[] = "board";
constexpr uint8_t kPowerConfig = 0x06;
constexpr uint8_t kBoost = 1U << 3;
constexpr uint8_t kGpioFunction = 0x16;
constexpr uint8_t kGpioMode = 0x10;
constexpr uint8_t kGpioOutput = 0x11;
constexpr uint8_t kGpioDrive = 0x13;
constexpr uint8_t kSpeakerBit = 1U << 3;

// Static mutex storage lives as long as Board, including failed-init retries.
class PowerLock {
public:
    explicit PowerLock(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~PowerLock() { xSemaphoreGive(mutex_); }
    PowerLock(const PowerLock&) = delete;
    PowerLock& operator=(const PowerLock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};

} // namespace

Board& Board::instance()
{
    static Board board;
    return board;
}

esp_err_t Board::initialize()
{
    if (!power_mutex_) {
        power_mutex_ = xSemaphoreCreateMutexStatic(&power_mutex_storage_);
        if (!power_mutex_) return ESP_ERR_NO_MEM;
    }
    PowerLock lock(power_mutex_);
    if (initialized_) return ESP_OK;
    if (pm1_ || i2c_bus_) {
        ESP_RETURN_ON_ERROR(deinitialize_locked(), kTag, "pending BSP cleanup");
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
    if (result == ESP_OK) {
        gpio_config_t keys = {};
        keys.pin_bit_mask = (1ULL << kKey1) | (1ULL << kKey2);
        keys.mode = GPIO_MODE_INPUT;
        keys.pull_up_en = GPIO_PULLUP_ENABLE;
        keys.pull_down_en = GPIO_PULLDOWN_DISABLE;
        keys.intr_type = GPIO_INTR_DISABLE;
        result = gpio_config(&keys);
    }
    if (result == ESP_OK) {
        // Only change the sleep nibble; preserve the configured I2C speed.
        result = update_pm1_register(0x09, 0, 0x0F);
        if (result != ESP_OK) {
            // PM1 can retain idle-sleep configuration across ESP resets. The
            // first I2C address wakes it without ACK; retry inside its 300 ms window.
            vTaskDelay(pdMS_TO_TICKS(10));
            result = update_pm1_register(0x09, 0, 0x0F);
        }
    }
    if (result == ESP_OK) result = set_speaker_locked(false);
    if (result != ESP_OK) {
        const esp_err_t cleanup = deinitialize_locked();
        if (cleanup != ESP_OK) {
            ESP_LOGE(kTag, "BSP rollback pending: %s", esp_err_to_name(cleanup));
        }
        return result;
    }
    initialized_ = true;
    ESP_LOGI(kTag, "StickS3 BSP ready (I2C SDA=%d SCL=%d)", kInternalI2cSda, kInternalI2cScl);
    return ESP_OK;
}

esp_err_t Board::enable_display_power()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    ESP_RETURN_ON_FALSE(initialized_ && pm1_, ESP_ERR_INVALID_STATE, kTag,
                        "board is not initialized");

    // M5PM1 GPIO_FUNC0 allocates TWO bits per pin: GPIO2 is bits 5:4.
    // Touching bit 2 here would instead alter GPIO1's function.
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioFunction, 0, 0x30), kTag,
                        "failed to select PM1 GPIO2 function");
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioMode, 1U << 2, 0), kTag,
                        "failed to set PM1 GPIO2 output mode");
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioDrive, 0, 1U << 2), kTag,
                        "failed to set PM1 GPIO2 push-pull mode");
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioOutput, 1U << 2, 0), kTag,
                        "failed to enable LCD L3B rail");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t Board::set_speaker_locked(bool enabled)
{
    // Set the latch LOW before changing function/direction, avoiding a stale
    // high latch when recovering from a previous firmware's GPIO setup.
    if (!enabled) {
        ESP_RETURN_ON_ERROR(update_pm1_register(kGpioOutput, 0, kSpeakerBit), kTag,
                            "failed to mute AW8737");
    }
    // GPIO3 is bits 7:6, not bit 3; preserve LCD GPIO2 and all other functions.
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioFunction, 0, 0xC0), kTag,
                        "failed to select PM1 GPIO3");
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioDrive, 0, kSpeakerBit), kTag,
                        "failed to configure AW8737 output");
    ESP_RETURN_ON_ERROR(update_pm1_register(kGpioMode, kSpeakerBit, 0), kTag,
                        "failed to configure AW8737 direction");
    if (enabled) {
        // Conservatively reserve the amplifier before the write: a bus error
        // may mean the PMIC accepted the write but its acknowledgement was lost.
        speaker_enabled_ = true;
        ESP_RETURN_ON_ERROR(update_pm1_register(kGpioOutput, kSpeakerBit, 0), kTag,
                            "failed to enable AW8737");
    } else {
        // >1 ms low puts AW8737 in shutdown; do not enable RMT RX before it settles.
        vTaskDelay(pdMS_TO_TICKS(2));
        speaker_enabled_ = false;
    }
    return ESP_OK;
}

esp_err_t Board::set_speaker_enabled(bool enabled)
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_) return ESP_ERR_INVALID_STATE;
    if (enabled && ir_receiver_active_) return ESP_ERR_INVALID_STATE;
    return set_speaker_locked(enabled);
}

esp_err_t Board::acquire_ir_receiver()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_ || speaker_enabled_ || ir_receiver_active_) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(set_speaker_locked(false), kTag, "IR requires a muted amplifier");
    ir_receiver_active_ = true;
    return ESP_OK;
}

esp_err_t Board::release_ir_receiver()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!ir_receiver_active_) return ESP_ERR_INVALID_STATE;
    ir_receiver_active_ = false;
    return ESP_OK;
}

esp_err_t Board::read_power_status_locked(PowerStatus& status)
{
    uint8_t config = 0;
    ESP_RETURN_ON_ERROR(read_pm1_register(kPowerConfig, config), kTag, "read power config");
    const uint8_t reg = 0x22;
    std::array<uint8_t, 6> raw{};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(pm1_, &reg, 1, raw.data(),
                                                   raw.size(), 100), kTag, "read power ADCs");
    PowerStatus value{};
    // PMIC's fixed ADC channels are continuously sampled and already in mV.
    value.battery_mv = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
    value.input_mv = static_cast<uint16_t>(raw[2] | (raw[3] << 8));
    value.external_mv = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
    value.boost_enabled = (config & kBoost) != 0;
    status = value;
    return ESP_OK;
}

esp_err_t Board::read_power_status(PowerStatus& status)
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_) return ESP_ERR_INVALID_STATE;
    return read_power_status_locked(status);
}

esp_err_t Board::ensure_5v_locked(bool& enabled_by_us)
{
    enabled_by_us = false;
    PowerStatus status{};
    ESP_RETURN_ON_ERROR(read_power_status_locked(status), kTag, "cannot determine IR supply");
    if (status.boost_enabled) return ESP_OK;
    // Existing input on Grove/HAT EXT_5V must NEVER be driven by the boost converter.
    // Reject ambiguous/brownout voltage instead of treating it as no external source.
    if (status.external_mv >= 4000) return ESP_OK;
    if (status.external_mv > 1000) return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(read_power_status_locked(status), kTag, "cannot recheck IR supply");
    if (status.boost_enabled || status.external_mv >= 4000) return ESP_OK;
    if (status.external_mv > 1000) return ESP_ERR_INVALID_STATE;

    // Track ownership BEFORE the write: if the acknowledgement is lost, cleanup
    // still turns off a boost we may have enabled.
    enabled_by_us = true;
    const esp_err_t result = update_pm1_register(kPowerConfig, kBoost, 0);
    if (result == ESP_OK) vTaskDelay(pdMS_TO_TICKS(30));
    return result;
}

esp_err_t Board::enable_5v_output()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_) return ESP_ERR_INVALID_STATE;
    bool changed = false;
    const esp_err_t result = ensure_5v_locked(changed);
    boost_owned_ = boost_owned_ || changed;
    if (result == ESP_OK) output_pinned_ = true;
    return result;
}

esp_err_t Board::acquire_ir_power()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_) return ESP_ERR_INVALID_STATE;
    if (ir_power_users_ == std::numeric_limits<uint32_t>::max()) return ESP_ERR_INVALID_STATE;
    bool changed = false;
    const esp_err_t result = ensure_5v_locked(changed);
    boost_owned_ = boost_owned_ || changed;
    if (result != ESP_OK) {
        if (changed) {
            // No lease was returned. Roll back here; if rollback also fails,
            // ownership remains recorded for Board::deinitialize()/next attempt.
            if (update_pm1_register(kPowerConfig, 0, kBoost) == ESP_OK) boost_owned_ = false;
        }
        return result;
    }
    ++ir_power_users_;
    return ESP_OK;
}

esp_err_t Board::release_ir_power()
{
    if (!power_mutex_) return ESP_ERR_INVALID_STATE;
    PowerLock lock(power_mutex_);
    if (!initialized_ || !pm1_ || ir_power_users_ == 0) return ESP_ERR_INVALID_STATE;
    if (ir_power_users_ == 1 && boost_owned_ && !output_pinned_) {
        ESP_RETURN_ON_ERROR(update_pm1_register(kPowerConfig, 0, kBoost), kTag,
                            "failed to restore external power input");
        boost_owned_ = false;
    }
    --ir_power_users_;
    return ESP_OK;
}

esp_err_t Board::deinitialize()
{
    if (!power_mutex_) return ESP_OK;
    PowerLock lock(power_mutex_);
    return deinitialize_locked();
}

esp_err_t Board::deinitialize_locked()
{
    // Never remove the shared I2C bus while an IR consumer still owns a lease.
    if (ir_power_users_ || ir_receiver_active_) return ESP_ERR_INVALID_STATE;
    initialized_ = false;
    if (pm1_) {
        ESP_RETURN_ON_ERROR(set_speaker_locked(false), kTag, "failed to mute during shutdown");
        if (boost_owned_) {
            ESP_RETURN_ON_ERROR(update_pm1_register(kPowerConfig, 0, kBoost), kTag,
                                "failed to restore boost during shutdown");
            boost_owned_ = false;
        }
        ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(pm1_), kTag, "failed to release PM1");
        pm1_ = nullptr;
    }
    // Retain handles and initialized state on failure so cleanup can be retried.
    if (i2c_bus_) {
        ESP_RETURN_ON_ERROR(i2c_del_master_bus(i2c_bus_), kTag, "failed to release I2C bus");
        i2c_bus_ = nullptr;
    }
    initialized_ = false;
    output_pinned_ = false;
    return ESP_OK;
}

esp_err_t Board::read_pm1_register(uint8_t reg, uint8_t& value)
{
    return i2c_master_transmit_receive(pm1_, &reg, 1, &value, 1, 100);
}

esp_err_t Board::update_pm1_register(uint8_t reg, uint8_t set_mask, uint8_t clear_mask)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(read_pm1_register(reg, value), kTag, "PM1 register 0x%02x read failed", reg);
    value = static_cast<uint8_t>((value | set_mask) & ~clear_mask);
    return write_pm1_register(reg, value);
}

esp_err_t Board::write_pm1_register(uint8_t reg, uint8_t value)
{
    const std::array<uint8_t, 2> data{reg, value};
    return i2c_master_transmit(pm1_, data.data(), data.size(), 100);
}

} // namespace bsp
