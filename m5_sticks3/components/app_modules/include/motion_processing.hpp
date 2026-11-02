#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "vqf.hpp"

namespace app_modules::motion {

inline constexpr float kSampleTimeSeconds = 0.005f;
inline constexpr float kDegreesToRadians = 0.017453292519943295f;
inline constexpr float kRadiansToDegrees = 57.29577951308232f;
inline constexpr float kGravity = 9.80665f;
inline constexpr size_t kFifoFrameBytes = 12;
inline constexpr size_t kMaxBatchFrames = 32;

inline VQFParams fusion_parameters()
{
    VQFParams parameters;
    // This filters acceleration in the inertial frame, not the output attitude.
    // 0.5 s shortens the inclination correction tail for handheld interaction.
    // Motion/rest bias estimation remains enabled; do not force angles to zero.
    parameters.tauAcc = 0.5;
    parameters.restThGyr = 1.5;
    parameters.restThAcc = 0.08;
    return parameters;
}

inline int16_t signed_le16(const uint8_t* bytes)
{
    const uint16_t value = static_cast<uint16_t>(bytes[0]) |
                           (static_cast<uint16_t>(bytes[1]) << 8);
    return static_cast<int16_t>(value < 0x8000 ? value : static_cast<int32_t>(value) - 65536);
}

inline int8_t gyro_cross_axis_factor(uint8_t value)
{
    value &= 0x7f;
    return static_cast<int8_t>(value < 0x40 ? value : static_cast<int>(value) - 128);
}

struct ImuSample {
    float accel_g[3]{};
    float gyro_dps[3]{};
};

inline bool decode_fifo_frame(const uint8_t* bytes, int8_t cross_axis, ImuSample& sample)
{
    // BMI270 headerless ACC+GYR FIFO order is GYR XYZ, then ACC XYZ.
    // Both sensors must use the same ODR and filtered, non-downsampled data.
    bool empty = true;
    for (size_t axis = 0; axis < 3; ++axis) {
        const int16_t gyro = signed_le16(bytes + 2 * axis);
        const int16_t accel = signed_le16(bytes + 6 + 2 * axis);
        empty &= gyro == INT16_MIN && accel == INT16_MIN;
        sample.gyro_dps[axis] = gyro * (1000.0f / 32768.0f);
        sample.accel_g[axis] = accel * (4.0f / 32768.0f);
    }
    // Bosch BMI270 datasheet 4.6.10: compensate the factory Z-to-X coupling.
    sample.gyro_dps[0] -= cross_axis * sample.gyro_dps[2] / 512.0f;
    return !empty;
}

inline void update_fusion(VQF& vqf, const ImuSample& sample)
{
    vqf_real_t gyro[3], accel[3];
    for (size_t axis = 0; axis < 3; ++axis) {
        gyro[axis] = sample.gyro_dps[axis] * kDegreesToRadians;
        accel[axis] = sample.accel_g[axis] * kGravity;
    }
    vqf.update(gyro, accel);
}

inline void quaternion_to_euler(const float quaternion[4], float& roll, float& pitch, float& yaw)
{
    const float w = quaternion[0], x = quaternion[1], y = quaternion[2], z = quaternion[3];
    roll = std::atan2(2.0f * (w * x + y * z),
                     1.0f - 2.0f * (x * x + y * y)) * kRadiansToDegrees;
    pitch = std::asin(std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f)) * kRadiansToDegrees;
    yaw = std::atan2(2.0f * (w * z + x * y),
                    1.0f - 2.0f * (y * y + z * z)) * kRadiansToDegrees;
}

} // namespace app_modules::motion
