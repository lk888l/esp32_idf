#pragma once

#include <cstddef>
#include <cstdint>

namespace hardware {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct Ws2812bConfig {
    int data_pin = -1;
    std::size_t led_count = 1;
    uint8_t brightness = 255;
};

class IWs2812bStrip {
public:
    virtual ~IWs2812bStrip() = default;

    virtual bool initialize() = 0;
    virtual void deinitialize() = 0;

    virtual void setPixel(std::size_t index, Rgb color) = 0;
    virtual void clear() = 0;
    virtual bool show() = 0;

    virtual std::size_t size() const = 0;
    virtual void setBrightness(uint8_t brightness) = 0;
    virtual uint8_t brightness() const = 0;
};

} // namespace hardware
