#pragma once

#include <cstdint>

#include "two_phase_bridge.hpp"

namespace motor_control {

struct VoltageVector {
    PhaseDuty duty{};
    uint16_t electrical_index = 0;
    uint16_t requested_amplitude_permille = 0;
    uint16_t applied_amplitude_permille = 0;
};

class StepperFoc final {
public:
    static constexpr uint16_t kElectricalCycleCounts = 1024;
    static constexpr uint16_t kQuarterCycleCounts = kElectricalCycleCounts / 4;
    static constexpr uint16_t kMaximumAmplitudePermille = 1000;
    static constexpr uint16_t kMaximumDuty = 1023;

    explicit StepperFoc(TwoPhaseBridge& bridge) noexcept
        : bridge_(bridge)
    {
    }

    VoltageVector calculate(uint32_t electrical_index,
                            uint16_t amplitude_permille) const noexcept;
    bool command(uint32_t electrical_index, uint16_t amplitude_permille) noexcept;
    void disable() noexcept;

private:
    static int16_t sineQ12(uint16_t electrical_index) noexcept;
    static int16_t dutyFromSine(int16_t sine_q12,
                                uint16_t amplitude_permille) noexcept;

    TwoPhaseBridge& bridge_;
};

} // namespace motor_control
