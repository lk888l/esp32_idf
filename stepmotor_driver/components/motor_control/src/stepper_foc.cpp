#include "stepper_foc.hpp"

#include <algorithm>
#include <array>

namespace motor_control {
namespace {

// One quarter of a 1024-point Q12 sine wave. The complete wave is reconstructed
// by symmetry, reducing the table from 2050 bytes to 514 bytes.
constexpr std::array<int16_t, StepperFoc::kQuarterCycleCounts + 1> kQuarterSineQ12{
    0, 25, 50, 75, 101, 126, 151, 176, 201, 226, 251, 276, 301, 326, 351, 376,
    401, 426, 451, 476, 501, 526, 551, 576, 601, 626, 651, 675, 700, 725, 750, 774,
    799, 824, 848, 873, 897, 922, 946, 971, 995, 1020, 1044, 1068, 1092, 1117,
    1141, 1165, 1189, 1213, 1237, 1261, 1285, 1309, 1332, 1356, 1380, 1404,
    1427, 1451, 1474, 1498, 1521, 1544, 1567, 1591, 1614, 1637, 1660, 1683,
    1706, 1729, 1751, 1774, 1797, 1819, 1842, 1864, 1886, 1909, 1931, 1953,
    1975, 1997, 2019, 2041, 2062, 2084, 2106, 2127, 2149, 2170, 2191, 2213,
    2234, 2255, 2276, 2296, 2317, 2338, 2359, 2379, 2399, 2420, 2440, 2460,
    2480, 2500, 2520, 2540, 2559, 2579, 2598, 2618, 2637, 2656, 2675, 2694,
    2713, 2732, 2751, 2769, 2788, 2806, 2824, 2843, 2861, 2878, 2896, 2914,
    2932, 2949, 2967, 2984, 3001, 3018, 3035, 3052, 3068, 3085, 3102, 3118,
    3134, 3150, 3166, 3182, 3198, 3214, 3229, 3244, 3260, 3275, 3290, 3305,
    3320, 3334, 3349, 3363, 3378, 3392, 3406, 3420, 3433, 3447, 3461, 3474,
    3487, 3500, 3513, 3526, 3539, 3551, 3564, 3576, 3588, 3600, 3612, 3624,
    3636, 3647, 3659, 3670, 3681, 3692, 3703, 3713, 3724, 3734, 3745, 3755,
    3765, 3775, 3784, 3794, 3803, 3812, 3822, 3831, 3839, 3848, 3857, 3865,
    3873, 3881, 3889, 3897, 3905, 3912, 3920, 3927, 3934, 3941, 3948, 3954,
    3961, 3967, 3973, 3979, 3985, 3991, 3996, 4002, 4007, 4012, 4017, 4022,
    4027, 4031, 4036, 4040, 4044, 4048, 4052, 4055, 4059, 4062, 4065, 4068,
    4071, 4074, 4076, 4079, 4081, 4083, 4085, 4087, 4088, 4090, 4091, 4092,
    4093, 4094, 4095, 4095, 4096, 4096, 4096,
};

constexpr uint8_t kQ12Shift = 12;
constexpr uint16_t kPermilleScale = 1000;

} // namespace

VoltageVector StepperFoc::calculate(uint32_t electrical_index,
                                    uint16_t amplitude_permille) const noexcept
{
    VoltageVector vector{};
    vector.electrical_index = static_cast<uint16_t>(electrical_index & 0x03FFU);
    vector.requested_amplitude_permille = amplitude_permille;
    vector.applied_amplitude_permille =
        std::min(amplitude_permille, kMaximumAmplitudePermille);

    const uint16_t phase_b_index = vector.electrical_index;
    const uint16_t phase_a_index =
        static_cast<uint16_t>((phase_b_index + kQuarterCycleCounts) & 0x03FFU);

    vector.duty.phase_a =
        dutyFromSine(sineQ12(phase_a_index), vector.applied_amplitude_permille);
    vector.duty.phase_b =
        dutyFromSine(sineQ12(phase_b_index), vector.applied_amplitude_permille);
    return vector;
}

bool StepperFoc::command(uint32_t electrical_index,
                         uint16_t amplitude_permille) noexcept
{
    return bridge_.write(calculate(electrical_index, amplitude_permille).duty);
}

void StepperFoc::disable() noexcept
{
    bridge_.disable();
}

int16_t StepperFoc::sineQ12(uint16_t electrical_index) noexcept
{
    const uint16_t wrapped = electrical_index & 0x03FFU;
    const uint16_t quadrant = wrapped >> 8U;
    const uint16_t offset = wrapped & 0x00FFU;

    switch (quadrant) {
    case 0:
        return kQuarterSineQ12[offset];
    case 1:
        return kQuarterSineQ12[kQuarterCycleCounts - offset];
    case 2:
        return static_cast<int16_t>(-kQuarterSineQ12[offset]);
    default:
        return static_cast<int16_t>(-kQuarterSineQ12[kQuarterCycleCounts - offset]);
    }
}

int16_t StepperFoc::dutyFromSine(int16_t sine_q12,
                                uint16_t amplitude_permille) noexcept
{
    const int32_t signed_sine = sine_q12;
    const uint32_t sine_magnitude =
        static_cast<uint32_t>(signed_sine < 0 ? -signed_sine : signed_sine);
    const uint32_t peak_duty =
        (static_cast<uint32_t>(amplitude_permille) * kMaximumDuty +
         (kPermilleScale / 2U)) /
        kPermilleScale;
    const int16_t duty = static_cast<int16_t>(
        (peak_duty * sine_magnitude) >> kQ12Shift);
    return signed_sine < 0 ? static_cast<int16_t>(-duty) : duty;
}

} // namespace motor_control
