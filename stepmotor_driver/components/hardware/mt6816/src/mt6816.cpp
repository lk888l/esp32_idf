#include "mt6816.hpp"

namespace hardware {
namespace {

constexpr uint8_t kAngleHighRegister = 0x03;
constexpr uint8_t kAngleLowRegister = 0x04;

} // namespace

Mt6816Reading Mt6816::read() noexcept
{
    Mt6816Reading reading{};

    for (uint8_t attempt = 1; attempt <= kMaximumAttempts; ++attempt) {
        reading.attempts = attempt;

        uint8_t high_byte = 0;
        uint8_t low_byte = 0;
        if (!registers_.readRegister(kAngleHighRegister, high_byte) ||
            !registers_.readRegister(kAngleLowRegister, low_byte)) {
            reading.status = Mt6816Status::transport_error;
            continue;
        }

        reading.raw_frame = static_cast<uint16_t>(
            (static_cast<uint16_t>(high_byte) << 8U) | low_byte);
        if (!hasEvenParity(reading.raw_frame)) {
            reading.status = Mt6816Status::parity_error;
            continue;
        }

        reading.angle = static_cast<uint16_t>(reading.raw_frame >> 2U);
        reading.no_magnet = (reading.raw_frame & (1U << 1U)) != 0;
        reading.status = Mt6816Status::ok;
        break;
    }

    last_reading_ = reading;
    return last_reading_;
}

bool Mt6816::hasEvenParity(uint16_t value) noexcept
{
    uint8_t ones = 0;
    while (value != 0) {
        ones = static_cast<uint8_t>(ones + (value & 0x01U));
        value >>= 1U;
    }
    return (ones & 0x01U) == 0;
}

} // namespace hardware
