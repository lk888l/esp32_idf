#pragma once

#include <array>
#include <cstdint>

namespace bsp::canfd
{

struct Frame
{
    uint32_t id;

    bool extended;
    bool rtr{false};
    bool fd_format;
    bool bitrate_switch{true};

    uint8_t dlc;

    std::array<uint8_t, 64> data;
};

}