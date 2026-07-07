#include "icm42688/icm42688.hpp"

#include <array>
#include <cmath>
#include <span>

namespace hardware {
namespace {

constexpr uint8_t kRegDeviceConfig = 0x11;
constexpr uint8_t kRegTempData1 = 0x1D;
constexpr uint8_t kRegPwrMgmt0 = 0x4E;
constexpr uint8_t kRegGyroConfig0 = 0x4F;
constexpr uint8_t kRegAccelConfig0 = 0x50;
constexpr uint8_t kRegWhoAmI = 0x75;
constexpr uint8_t kRegBankSel = 0x76;

constexpr uint8_t kExpectedWhoAmI = 0x47;
constexpr uint8_t kSoftReset = 0x01;
constexpr uint8_t kBank0 = 0x00;
constexpr uint8_t kGyroAccelLowNoise = 0x0F;
constexpr uint8_t kFs2000DpsOdr1Khz = 0x06;
constexpr uint8_t kFs16gOdr1Khz = 0x06;

constexpr float kAccelScaleGPerLsb = 16.0f / 32768.0f;
constexpr float kGyroScaleDpsPerLsb = 2000.0f / 32768.0f;
constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kQuaternionNormEpsilon = 1.0e-6f;

} // namespace

const char* toString(Icm42688Status status)
{
    switch (status) {
    case Icm42688Status::ok:
        return "ok";
    case Icm42688Status::invalid_argument:
        return "invalid_argument";
    case Icm42688Status::invalid_state:
        return "invalid_state";
    case Icm42688Status::bus_error:
        return "bus_error";
    case Icm42688Status::device_not_found:
        return "device_not_found";
    default:
        return "unknown";
    }
}

Icm42688Status quaternionToEulerAngles(const Quaternion& quaternion, EulerAngles& angles)
{
    const float norm_sq = quaternion.w * quaternion.w +
                          quaternion.x * quaternion.x +
                          quaternion.y * quaternion.y +
                          quaternion.z * quaternion.z;
    if (norm_sq <= kQuaternionNormEpsilon) {
        return Icm42688Status::invalid_argument;
    }

    const float inv_norm = 1.0f / std::sqrt(norm_sq);
    const float w = quaternion.w * inv_norm;
    const float x = quaternion.x * inv_norm;
    const float y = quaternion.y * inv_norm;
    const float z = quaternion.z * inv_norm;

    const float roll_sin = 2.0f * (w * x + y * z);
    const float roll_cos = 1.0f - 2.0f * (x * x + y * y);
    angles.roll_deg = std::atan2(roll_sin, roll_cos) * kRadToDeg;

    float pitch_sin = 2.0f * (w * y - z * x);
    if (pitch_sin > 1.0f) {
        pitch_sin = 1.0f;
    } else if (pitch_sin < -1.0f) {
        pitch_sin = -1.0f;
    }
    angles.pitch_deg = std::asin(pitch_sin) * kRadToDeg;

    const float yaw_sin = 2.0f * (w * z + x * y);
    const float yaw_cos = 1.0f - 2.0f * (y * y + z * z);
    angles.yaw_deg = std::atan2(yaw_sin, yaw_cos) * kRadToDeg;

    return Icm42688Status::ok;
}

Icm42688::Icm42688(bsp::I2CDevice& device, const Icm42688Config& config)
    : device_(device)
    , config_(config)
{
}

Icm42688Status Icm42688::initialize()
{
    if (config_.delay_ms == nullptr) {
        return Icm42688Status::invalid_argument;
    }
    if (initialized_) {
        return Icm42688Status::ok;
    }

    Icm42688Status status = writeRegister(kRegDeviceConfig, kSoftReset);
    if (status != Icm42688Status::ok) {
        return status;
    }
    delay(20);

    uint8_t who_am_i = 0;
    status = readWhoAmI(who_am_i);
    if (status != Icm42688Status::ok) {
        return status;
    }
    if (who_am_i != kExpectedWhoAmI) {
        return Icm42688Status::device_not_found;
    }

    status = writeRegister(kRegBankSel, kBank0);
    if (status != Icm42688Status::ok) {
        return status;
    }

    status = writeRegister(kRegGyroConfig0, kFs2000DpsOdr1Khz);
    if (status != Icm42688Status::ok) {
        return status;
    }

    status = writeRegister(kRegAccelConfig0, kFs16gOdr1Khz);
    if (status != Icm42688Status::ok) {
        return status;
    }

    status = writeRegister(kRegPwrMgmt0, kGyroAccelLowNoise);
    if (status != Icm42688Status::ok) {
        return status;
    }
    delay(50);

    initialized_ = true;
    return Icm42688Status::ok;
}

Icm42688Status Icm42688::readSample(Icm42688Sample& sample)
{
    if (!initialized_) {
        return Icm42688Status::invalid_state;
    }

    uint8_t data[14] = {};
    const Icm42688Status status = readRegisters(kRegTempData1, data, sizeof(data));
    if (status != Icm42688Status::ok) {
        return status;
    }

    const int16_t temp_raw = be16(&data[0]);
    const int16_t accel_x = be16(&data[2]);
    const int16_t accel_y = be16(&data[4]);
    const int16_t accel_z = be16(&data[6]);
    const int16_t gyro_x = be16(&data[8]);
    const int16_t gyro_y = be16(&data[10]);
    const int16_t gyro_z = be16(&data[12]);

    sample.temperature_c = (static_cast<float>(temp_raw) / 132.48f) + 25.0f;
    sample.accel_x_g = static_cast<float>(accel_x) * kAccelScaleGPerLsb;
    sample.accel_y_g = static_cast<float>(accel_y) * kAccelScaleGPerLsb;
    sample.accel_z_g = static_cast<float>(accel_z) * kAccelScaleGPerLsb;
    sample.gyro_x_dps = static_cast<float>(gyro_x) * kGyroScaleDpsPerLsb;
    sample.gyro_y_dps = static_cast<float>(gyro_y) * kGyroScaleDpsPerLsb;
    sample.gyro_z_dps = static_cast<float>(gyro_z) * kGyroScaleDpsPerLsb;

    return Icm42688Status::ok;
}

Icm42688Status Icm42688::readWhoAmI(uint8_t& value)
{
    return readRegister(kRegWhoAmI, value);
}

Icm42688Status Icm42688::writeRegister(uint8_t reg, uint8_t value)
{
    const std::array<uint8_t, 2> data = {reg, value};
    return fromI2CStatus(device_.write(data));
}

Icm42688Status Icm42688::readRegister(uint8_t reg, uint8_t& value)
{
    return readRegisters(reg, &value, 1);
}

Icm42688Status Icm42688::readRegisters(uint8_t start_reg, uint8_t* data, size_t length)
{
    if (data == nullptr || length == 0) {
        return Icm42688Status::invalid_argument;
    }

    return fromI2CStatus(device_.writeRead(
        std::span<const uint8_t>(&start_reg, 1),
        std::span<uint8_t>(data, length)));
}

void Icm42688::delay(uint32_t ms) const
{
    config_.delay_ms(ms);
}

Icm42688Status Icm42688::fromI2CStatus(bsp::I2CStatus status)
{
    switch (status) {
    case bsp::I2CStatus::ok:
        return Icm42688Status::ok;
    case bsp::I2CStatus::invalid_argument:
        return Icm42688Status::invalid_argument;
    case bsp::I2CStatus::invalid_state:
        return Icm42688Status::invalid_state;
    case bsp::I2CStatus::not_found:
        return Icm42688Status::device_not_found;
    case bsp::I2CStatus::timeout:
    case bsp::I2CStatus::io_error:
    default:
        return Icm42688Status::bus_error;
    }
}

int16_t Icm42688::be16(const uint8_t* data)
{
    return static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

} // namespace hardware
