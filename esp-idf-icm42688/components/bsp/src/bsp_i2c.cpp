#include "bsp_i2c.hpp"

namespace bsp {

const char* toString(I2CStatus status)
{
    switch (status) {
    case I2CStatus::ok:
        return "ok";
    case I2CStatus::invalid_argument:
        return "invalid_argument";
    case I2CStatus::invalid_state:
        return "invalid_state";
    case I2CStatus::not_found:
        return "not_found";
    case I2CStatus::timeout:
        return "timeout";
    case I2CStatus::io_error:
        return "io_error";
    default:
        return "unknown";
    }
}

} // namespace bsp
