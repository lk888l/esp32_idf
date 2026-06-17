#pragma once

#include <functional>
#include <array>
#include <cstdint>
#include "BasicObject.hpp"

namespace bsp::canfd
{

struct Frame
{
    uint32_t id;

    bool extended{false};
    bool rtr{false};
    bool fd_format{false};
    bool bitrate_switch{true};

    uint8_t dlc;

    std::array<uint8_t, 64> data;
};

}

class ICanDriver : public BasicObject
{
public:
    virtual ~ICanDriver() = default;

    virtual bool init() = 0;

    virtual bool start() = 0;

    virtual bool stop() = 0;

    virtual bool send(const bsp::canfd::Frame& frame) = 0;
    virtual void signal_RxComplete(std::function<void(bsp::canfd::Frame&)> slot) = 0;
};