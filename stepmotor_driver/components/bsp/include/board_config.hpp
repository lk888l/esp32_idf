 #pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hal/gpio_types.h"
#include "sdkconfig.h"

namespace board {

// TB6612FNG control pins. STBY is on a non-strapping pin and stays low until
// every direction and PWM output is configured. GPIO8 is a strapping/on-board
// LED pin, but its value does not prevent normal SPI boot when GPIO9 is high.
inline constexpr gpio_num_t kTb6612PwmAGpio = GPIO_NUM_0;
inline constexpr gpio_num_t kTb6612PwmBGpio = GPIO_NUM_1;
inline constexpr gpio_num_t kTb6612Ain1Gpio = GPIO_NUM_3;
inline constexpr gpio_num_t kTb6612Ain2Gpio = GPIO_NUM_8;
inline constexpr gpio_num_t kTb6612Bin1Gpio = GPIO_NUM_20;
inline constexpr gpio_num_t kTb6612Bin2Gpio = GPIO_NUM_21;
inline constexpr gpio_num_t kTb6612StandbyGpio = GPIO_NUM_10;

inline constexpr gpio_num_t kEncoderSclkGpio = GPIO_NUM_4;
inline constexpr gpio_num_t kEncoderMisoGpio = GPIO_NUM_5;
inline constexpr gpio_num_t kEncoderMosiGpio = GPIO_NUM_6;
inline constexpr gpio_num_t kEncoderChipSelectGpio = GPIO_NUM_7;

inline constexpr uint32_t kPwmFrequencyHz = 20'000;
inline constexpr uint8_t kPwmResolutionBits = 10;
inline constexpr uint32_t kEncoderSpiClockHz = 4'000'000;
inline constexpr uint8_t kEncoderSpiMode = 1;

// Safe default: encoder monitoring is enabled, motor energizing is not.
inline constexpr bool kOpenLoopDemoEnabled = false;
inline constexpr uint16_t kDemoAmplitudePermille = 100;
inline constexpr uint16_t kDemoElectricalStepPerTick = 1;
inline constexpr uint32_t kDemoUpdatePeriodMs = 1;
inline constexpr uint32_t kEncoderMonitorPeriodMs = 50;

template <std::size_t Size>
constexpr bool pinsAreUnique(const std::array<gpio_num_t, Size>& pins) noexcept
{
    for (std::size_t left = 0; left < pins.size(); ++left) {
        for (std::size_t right = left + 1; right < pins.size(); ++right) {
            if (pins[left] == pins[right]) {
                return false;
            }
        }
    }
    return true;
}

inline constexpr std::array<gpio_num_t, 11> kAssignedPins{
    kTb6612PwmAGpio,
    kTb6612PwmBGpio,
    kTb6612Ain1Gpio,
    kTb6612Ain2Gpio,
    kTb6612Bin1Gpio,
    kTb6612Bin2Gpio,
    kTb6612StandbyGpio,
    kEncoderSclkGpio,
    kEncoderMisoGpio,
    kEncoderMosiGpio,
    kEncoderChipSelectGpio,
};

static_assert(pinsAreUnique(kAssignedPins), "board pin assignments must be unique");
static_assert(kDemoAmplitudePermille <= 1000,
              "demo amplitude exceeds the supported command range");
static_assert(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG,
              "GPIO20/GPIO21 require the primary console on USB Serial/JTAG");

} // namespace board
