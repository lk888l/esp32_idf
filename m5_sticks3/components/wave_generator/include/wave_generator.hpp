#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"

namespace wave {

inline constexpr std::array<uint32_t, 8> kFrequencyPresetsHz = {
    100'000,
    250'000,
    500'000,
    1'000'000,
    2'000'000,
    3'000'000,
    4'000'000,
    5'000'000,
};

inline constexpr std::array<uint8_t, 3> kDutyPresetsPercent = {25, 50, 75};

class Generator {
public:
    static Generator& instance();

    esp_err_t initialize();
    esp_err_t deinitialize();
    esp_err_t set_frequency(uint32_t frequency_hz);
    esp_err_t set_duty_percent(uint8_t duty_percent);
    esp_err_t set_enabled(bool enabled);

    bool initialized() const { return initialized_; }
    bool enabled() const { return enabled_; }
    uint32_t frequency_hz() const { return frequency_hz_; }
    uint8_t duty_percent() const { return duty_percent_; }

private:
    Generator() = default;

    esp_err_t configure_channel(int gpio, int channel, uint32_t duty);
    esp_err_t set_duty(uint32_t duty);

    bool timer_initialized_ = false;
    bool channel_g4_initialized_ = false;
    bool channel_g5_initialized_ = false;
    bool initialized_ = false;
    bool enabled_ = false;
    uint32_t frequency_hz_ = 1'000'000;
    uint8_t duty_percent_ = 50;
    uint32_t duty_ticks_ = 2;
};

} // namespace wave
