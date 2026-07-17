#include <cassert>
#include <cstdint>

#include "stepper_foc.hpp"

namespace {

class FakeBridge final : public motor_control::TwoPhaseBridge {
public:
    bool write(const motor_control::PhaseDuty& duty) noexcept override
    {
        last_duty = duty;
        ++write_count;
        return write_result;
    }

    void disable() noexcept override
    {
        disabled = true;
        last_duty = {};
    }

    motor_control::PhaseDuty last_duty{};
    uint32_t write_count = 0;
    bool write_result = true;
    bool disabled = false;
};

} // namespace

int main()
{
    FakeBridge bridge;
    motor_control::StepperFoc foc{bridge};

    const auto zero_degrees = foc.calculate(0, 1000);
    assert(zero_degrees.duty.phase_a == 1023);
    assert(zero_degrees.duty.phase_b == 0);

    const auto ninety_degrees = foc.calculate(256, 1000);
    assert(ninety_degrees.duty.phase_a == 0);
    assert(ninety_degrees.duty.phase_b == 1023);

    const auto one_eighty_degrees = foc.calculate(512, 1000);
    assert(one_eighty_degrees.duty.phase_a == -1023);
    assert(one_eighty_degrees.duty.phase_b == 0);

    const auto two_seventy_degrees = foc.calculate(768, 1000);
    assert(two_seventy_degrees.duty.phase_a == 0);
    assert(two_seventy_degrees.duty.phase_b == -1023);

    const auto disabled_vector = foc.calculate(123, 0);
    assert(disabled_vector.duty.phase_a == 0);
    assert(disabled_vector.duty.phase_b == 0);

    const auto ten_percent_vector = foc.calculate(0, 100);
    assert(ten_percent_vector.duty.phase_a == 102);
    assert(ten_percent_vector.duty.phase_b == 0);

    const auto clamped_vector = foc.calculate(0, 1200);
    assert(clamped_vector.requested_amplitude_permille == 1200);
    assert(clamped_vector.applied_amplitude_permille == 1000);
    assert(clamped_vector.duty.phase_a == 1023);

    assert(foc.command(256, 500));
    assert(bridge.write_count == 1);
    assert(bridge.last_duty.phase_a == 0);
    assert(bridge.last_duty.phase_b > 0);

    foc.disable();
    assert(bridge.disabled);
    assert(bridge.last_duty.phase_a == 0);
    assert(bridge.last_duty.phase_b == 0);
    return 0;
}
