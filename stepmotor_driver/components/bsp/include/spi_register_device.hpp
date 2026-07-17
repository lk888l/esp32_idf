#pragma once

#include <cstdint>

namespace bsp {

class SpiRegisterDevice {
public:
    virtual ~SpiRegisterDevice() = default;

    virtual bool readRegister(uint8_t register_address, uint8_t& value) noexcept = 0;
};

} // namespace bsp
