#pragma once

#include <cstdint>

#include "driver/ledc.h"
#include "hal/gpio_types.h"

#include "two_phase_bridge.hpp"

namespace bsp {

struct Tb6612BridgeConfig {
    gpio_num_t phase_a_pwm_gpio = GPIO_NUM_NC;
    gpio_num_t phase_a_in1_gpio = GPIO_NUM_NC;
    gpio_num_t phase_a_in2_gpio = GPIO_NUM_NC;
    gpio_num_t phase_b_pwm_gpio = GPIO_NUM_NC;
    gpio_num_t phase_b_in1_gpio = GPIO_NUM_NC;
    gpio_num_t phase_b_in2_gpio = GPIO_NUM_NC;
    gpio_num_t standby_gpio = GPIO_NUM_NC;
    uint32_t pwm_frequency_hz = 20'000;
    bool invert_phase_a = false;
    bool invert_phase_b = false;
};

class EspTb6612Bridge final : public motor_control::TwoPhaseBridge {
public:
    explicit EspTb6612Bridge(const Tb6612BridgeConfig& config) noexcept;
    ~EspTb6612Bridge() override;

    bool initialize() noexcept;
    void deinitialize() noexcept;
    bool isInitialized() const noexcept { return initialized_; }

    bool write(const motor_control::PhaseDuty& duty) noexcept override;
    void disable() noexcept override;

private:
    enum class Direction : int8_t {
        reverse = -1,
        stop = 0,
        forward = 1,
    };

    static constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
    static constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_10_BIT;
    static constexpr uint32_t kMaximumDuty = 1023;

    bool configureOutput(gpio_num_t gpio) noexcept;
    bool configurePwmChannel(ledc_channel_t channel, gpio_num_t gpio) noexcept;
    bool applyPhase(ledc_channel_t pwm_channel,
                    gpio_num_t in1_gpio,
                    gpio_num_t in2_gpio,
                    int16_t signed_duty,
                    bool invert,
                    Direction& last_direction) noexcept;
    bool setDirection(gpio_num_t in1_gpio,
                      gpio_num_t in2_gpio,
                      Direction direction) noexcept;
    bool setDuty(ledc_channel_t channel, uint32_t duty) noexcept;

    Tb6612BridgeConfig config_;
    Direction phase_a_direction_ = Direction::stop;
    Direction phase_b_direction_ = Direction::stop;
    bool standby_enabled_ = false;
    bool initialized_ = false;
};

} // namespace bsp
