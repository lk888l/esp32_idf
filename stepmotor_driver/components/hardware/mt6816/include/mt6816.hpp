#pragma once

#include <cstdint>

#include "spi_register_device.hpp"

namespace hardware {

enum class Mt6816Status : uint8_t {
    ok = 0,
    transport_error,
    parity_error,
};

struct Mt6816Reading {
    Mt6816Status status = Mt6816Status::transport_error;
    uint16_t raw_frame = 0;
    uint16_t angle = 0;
    bool no_magnet = false;
    uint8_t attempts = 0;

    bool valid() const noexcept { return status == Mt6816Status::ok; }
};

class Mt6816 final {
public:
    static constexpr uint16_t kResolution = 16384;
    static constexpr uint8_t kMaximumAttempts = 3;

    explicit Mt6816(bsp::SpiRegisterDevice& registers) noexcept
        : registers_(registers)
    {
    }

    Mt6816Reading read() noexcept;
    const Mt6816Reading& lastReading() const noexcept { return last_reading_; }

private:
    static bool hasEvenParity(uint16_t value) noexcept;

    bsp::SpiRegisterDevice& registers_;
    Mt6816Reading last_reading_{};
};

} // namespace hardware
