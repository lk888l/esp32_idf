#pragma once

#include <cstddef>
#include <cstdint>

#include "bsp_i2c.hpp"

namespace hardware {

enum class Icm42688Status : uint8_t {
    ok = 0,
    invalid_argument,
    invalid_state,
    bus_error,
    device_not_found,
};

const char* toString(Icm42688Status status);

using DelayMs = void (*)(uint32_t ms);

struct Icm42688Config {
    DelayMs delay_ms = nullptr;
};

struct Icm42688Sample {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float temperature_c;
};

struct Quaternion {
    float w;
    float x;
    float y;
    float z;
};

struct EulerAngles {
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
};

Icm42688Status quaternionToEulerAngles(const Quaternion& quaternion, EulerAngles& angles);

class Icm42688 {
public:
    Icm42688(bsp::I2CDevice& device, const Icm42688Config& config);

    Icm42688(const Icm42688&) = delete;
    Icm42688& operator=(const Icm42688&) = delete;

    Icm42688Status initialize();
    Icm42688Status readSample(Icm42688Sample& sample);
    Icm42688Status readWhoAmI(uint8_t& value);

    bool isInitialized() const { return initialized_; }

private:
    Icm42688Status writeRegister(uint8_t reg, uint8_t value);
    Icm42688Status readRegister(uint8_t reg, uint8_t& value);
    Icm42688Status readRegisters(uint8_t start_reg, uint8_t* data, size_t length);
    void delay(uint32_t ms) const;

    static Icm42688Status fromI2CStatus(bsp::I2CStatus status);
    static int16_t be16(const uint8_t* data);

    bsp::I2CDevice& device_;
    Icm42688Config config_;
    bool initialized_ = false;
};

} // namespace hardware
