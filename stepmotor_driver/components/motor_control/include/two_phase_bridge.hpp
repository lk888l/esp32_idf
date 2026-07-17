#pragma once

#include <cstdint>

namespace motor_control {

struct PhaseDuty {
    int16_t phase_a = 0;
    int16_t phase_b = 0;
};

class TwoPhaseBridge {
public:
    virtual ~TwoPhaseBridge() = default;

    virtual bool write(const PhaseDuty& duty) noexcept = 0;
    virtual void disable() noexcept = 0;
};

} // namespace motor_control
