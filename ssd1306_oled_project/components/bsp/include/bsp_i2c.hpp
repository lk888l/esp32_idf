#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace bsp {

enum class I2CStatus : std::uint8_t {
    ok = 0,
    invalid_argument,
    invalid_state,
    not_found,
    timeout,
    io_error,
};

const char* toString(I2CStatus status);

class I2CDevice {
public:
    virtual ~I2CDevice() = default;

    virtual I2CStatus write(std::span<const std::uint8_t> data) = 0;
    virtual I2CStatus writeRead(std::span<const std::uint8_t> write_data,
                                std::span<std::uint8_t> read_data) = 0;
};

struct I2CDeviceResult {
    I2CStatus status = I2CStatus::invalid_state;
    std::unique_ptr<I2CDevice> device;

    explicit operator bool() const
    {
        return status == I2CStatus::ok && device != nullptr;
    }
};

class I2CBus {
public:
    virtual ~I2CBus() = default;

    virtual I2CStatus init() = 0;
    virtual I2CStatus deinit() = 0;
    virtual I2CDeviceResult createDevice(std::uint8_t address,
                                         std::uint32_t clock_hz) = 0;
    virtual bool isInitialized() const = 0;
};

} // namespace bsp
