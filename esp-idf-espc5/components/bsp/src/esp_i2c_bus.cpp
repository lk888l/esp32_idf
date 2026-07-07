#include "esp_i2c_bus.hpp"

#include <array>
#include <span>

#include "esp_err.h"

namespace bsp {
namespace {

constexpr int kTransactionTimeoutMs = 100;

I2CStatus fromEspError(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return I2CStatus::ok;
    case ESP_ERR_INVALID_ARG:
        return I2CStatus::invalid_argument;
    case ESP_ERR_INVALID_STATE:
        return I2CStatus::invalid_state;
    case ESP_ERR_NOT_FOUND:
        return I2CStatus::not_found;
    case ESP_ERR_TIMEOUT:
        return I2CStatus::timeout;
    default:
        return I2CStatus::io_error;
    }
}

class EspI2CDevice final : public I2CDevice {
public:
    explicit EspI2CDevice(i2c_master_dev_handle_t handle)
        : handle_(handle)
    {
    }

    ~EspI2CDevice() override
    {
        if (handle_ != nullptr) {
            i2c_master_bus_rm_device(handle_);
            handle_ = nullptr;
        }
    }

    EspI2CDevice(const EspI2CDevice&) = delete;
    EspI2CDevice& operator=(const EspI2CDevice&) = delete;

    I2CStatus write(std::span<const uint8_t> data) override
    {
        if (handle_ == nullptr || data.empty()) {
            return I2CStatus::invalid_argument;
        }

        return fromEspError(i2c_master_transmit(
            handle_,
            data.data(),
            data.size(),
            kTransactionTimeoutMs));
    }

    I2CStatus writeRead(std::span<const uint8_t> write_data,
                        std::span<uint8_t> read_data) override
    {
        if (handle_ == nullptr || write_data.empty() || read_data.empty()) {
            return I2CStatus::invalid_argument;
        }

        return fromEspError(i2c_master_transmit_receive(
            handle_,
            write_data.data(),
            write_data.size(),
            read_data.data(),
            read_data.size(),
            kTransactionTimeoutMs));
    }

private:
    i2c_master_dev_handle_t handle_ = nullptr;
};

} // namespace

EspI2CBus::EspI2CBus(const EspI2CBusConfig& config)
    : config_(config)
{
}

EspI2CBus::~EspI2CBus()
{
    deinit();
}

I2CStatus EspI2CBus::init()
{
    if (bus_ != nullptr) {
        return I2CStatus::ok;
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = config_.port;
    bus_config.sda_io_num = config_.sda;
    bus_config.scl_io_num = config_.scl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = config_.enable_internal_pullups;

    return fromEspError(i2c_new_master_bus(&bus_config, &bus_));
}

I2CStatus EspI2CBus::deinit()
{
    if (bus_ == nullptr) {
        return I2CStatus::ok;
    }

    const I2CStatus status = fromEspError(i2c_del_master_bus(bus_));
    if (status == I2CStatus::ok) {
        bus_ = nullptr;
    }
    return status;
}

I2CDeviceResult EspI2CBus::createDevice(uint8_t address, uint32_t clock_hz)
{
    if (bus_ == nullptr || clock_hz == 0) {
        return {I2CStatus::invalid_state, nullptr};
    }

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = address;
    device_config.scl_speed_hz = clock_hz;

    i2c_master_dev_handle_t handle = nullptr;
    const I2CStatus status = fromEspError(i2c_master_bus_add_device(bus_, &device_config, &handle));
    if (status != I2CStatus::ok) {
        return {status, nullptr};
    }

    return {I2CStatus::ok, std::make_unique<EspI2CDevice>(handle)};
}

} // namespace bsp
