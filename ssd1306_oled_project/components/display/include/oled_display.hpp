#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "bsp_i2c.hpp"

extern "C" {
#include "u8g2.h"
}

namespace display {

enum class DisplayStatus : std::uint8_t {
    ok = 0,
    invalid_argument,
    not_initialized,
    buffer_overflow,
    transport_error,
};

const char* toString(DisplayStatus status);

enum class Rotation : std::uint8_t {
    deg0,
    deg90,
    deg180,
    deg270,
};

struct OledDisplayConfig {
    Rotation rotation = Rotation::deg0;
    std::uint8_t contrast = 0xCF;
    void (*delay_ms)(std::uint32_t) = nullptr;
    void (*delay_us)(std::uint32_t) = nullptr;
};

class OledDisplay {
public:
    static constexpr std::uint16_t kNativeWidth = 128;
    static constexpr std::uint16_t kNativeHeight = 64;

    OledDisplay(bsp::I2CDevice& device, OledDisplayConfig config);

    DisplayStatus initialize();
    DisplayStatus present();
    DisplayStatus setPowerSave(bool enabled);
    DisplayStatus setContrast(std::uint8_t contrast);

    bool isInitialized() const { return initialized_; }
    DisplayStatus lastStatus() const { return last_status_; }
    std::uint16_t width() const;
    std::uint16_t height() const;

    u8g2_t& nativeHandle() { return u8g2_; }
    const std::uint8_t* framebufferData() const;
    std::size_t framebufferSize() const;

private:
    static std::uint8_t byteCallback(u8x8_t* u8x8,
                                     std::uint8_t message,
                                     std::uint8_t value,
                                     void* data);
    static std::uint8_t gpioDelayCallback(u8x8_t* u8x8,
                                          std::uint8_t message,
                                          std::uint8_t value,
                                          void* data);

    std::uint8_t onByteMessage(std::uint8_t message,
                               std::uint8_t count,
                               const void* data);
    std::uint8_t onDelayMessage(std::uint8_t message, std::uint8_t value);
    void resetStatus();

    bsp::I2CDevice& device_;
    OledDisplayConfig config_;
    u8g2_t u8g2_{};
    std::array<std::uint8_t, 64> transfer_buffer_{};
    std::size_t transfer_size_ = 0;
    DisplayStatus last_status_ = DisplayStatus::not_initialized;
    bool initialized_ = false;
};

} // namespace display
