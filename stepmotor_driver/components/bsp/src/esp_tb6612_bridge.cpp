#include "esp_tb6612_bridge.hpp"

#include <algorithm>

#include "driver/gpio.h"

namespace bsp {
namespace {

constexpr ledc_channel_t kPhaseAPwmChannel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kPhaseBPwmChannel = LEDC_CHANNEL_1;

} // namespace

EspTb6612Bridge::EspTb6612Bridge(const Tb6612BridgeConfig& config) noexcept
    : config_(config)
{
}

EspTb6612Bridge::~EspTb6612Bridge()
{
    deinitialize();
}

bool EspTb6612Bridge::initialize() noexcept
{
    if (initialized_) {
        return true;
    }

    // TB6612 has an internal 200 kOhm STBY pull-down. Configure and hold it low
    // before any direction or PWM pin can become active.
    if (!configureOutput(config_.standby_gpio) ||
        gpio_set_level(config_.standby_gpio, 0) != ESP_OK ||
        !configureOutput(config_.phase_a_in1_gpio) ||
        !configureOutput(config_.phase_a_in2_gpio) ||
        !configureOutput(config_.phase_b_in1_gpio) ||
        !configureOutput(config_.phase_b_in2_gpio) ||
        !setDirection(config_.phase_a_in1_gpio,
                      config_.phase_a_in2_gpio,
                      Direction::stop) ||
        !setDirection(config_.phase_b_in1_gpio,
                      config_.phase_b_in2_gpio,
                      Direction::stop)) {
        deinitialize();
        return false;
    }

    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = kSpeedMode;
    timer_config.duty_resolution = kDutyResolution;
    timer_config.timer_num = kTimer;
    timer_config.freq_hz = config_.pwm_frequency_hz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;

    if (ledc_timer_config(&timer_config) != ESP_OK ||
        !configurePwmChannel(kPhaseAPwmChannel, config_.phase_a_pwm_gpio) ||
        !configurePwmChannel(kPhaseBPwmChannel, config_.phase_b_pwm_gpio)) {
        deinitialize();
        return false;
    }

    initialized_ = true;
    disable();
    return true;
}

void EspTb6612Bridge::deinitialize() noexcept
{
    disable();

    ledc_stop(kSpeedMode, kPhaseAPwmChannel, 0);
    ledc_stop(kSpeedMode, kPhaseBPwmChannel, 0);

    gpio_reset_pin(config_.phase_a_pwm_gpio);
    gpio_reset_pin(config_.phase_a_in1_gpio);
    gpio_reset_pin(config_.phase_a_in2_gpio);
    gpio_reset_pin(config_.phase_b_pwm_gpio);
    gpio_reset_pin(config_.phase_b_in1_gpio);
    gpio_reset_pin(config_.phase_b_in2_gpio);
    gpio_reset_pin(config_.standby_gpio);

    initialized_ = false;
}

bool EspTb6612Bridge::write(const motor_control::PhaseDuty& duty) noexcept
{
    if (!initialized_) {
        return false;
    }
    if (duty.phase_a == 0 && duty.phase_b == 0) {
        disable();
        return true;
    }

    const bool phases_ready =
        applyPhase(kPhaseAPwmChannel,
                   config_.phase_a_in1_gpio,
                   config_.phase_a_in2_gpio,
                   duty.phase_a,
                   config_.invert_phase_a,
                   phase_a_direction_) &&
        applyPhase(kPhaseBPwmChannel,
                   config_.phase_b_in1_gpio,
                   config_.phase_b_in2_gpio,
                   duty.phase_b,
                   config_.invert_phase_b,
                   phase_b_direction_);

    if (!phases_ready) {
        disable();
        return false;
    }

    if (!standby_enabled_) {
        if (gpio_set_level(config_.standby_gpio, 1) != ESP_OK) {
            disable();
            return false;
        }
        standby_enabled_ = true;
    }
    return true;
}

void EspTb6612Bridge::disable() noexcept
{
    // Disable the power stage first, then clear PWM and direction state.
    gpio_set_level(config_.standby_gpio, 0);
    standby_enabled_ = false;

    setDuty(kPhaseAPwmChannel, 0);
    setDuty(kPhaseBPwmChannel, 0);
    setDirection(config_.phase_a_in1_gpio, config_.phase_a_in2_gpio, Direction::stop);
    setDirection(config_.phase_b_in1_gpio, config_.phase_b_in2_gpio, Direction::stop);
    phase_a_direction_ = Direction::stop;
    phase_b_direction_ = Direction::stop;
}

bool EspTb6612Bridge::configureOutput(gpio_num_t gpio) noexcept
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<uint32_t>(gpio);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK;
}

bool EspTb6612Bridge::configurePwmChannel(ledc_channel_t channel,
                                         gpio_num_t gpio) noexcept
{
    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = gpio;
    channel_config.speed_mode = kSpeedMode;
    channel_config.channel = channel;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = kTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    channel_config.flags.output_invert = 0;
    return ledc_channel_config(&channel_config) == ESP_OK;
}

bool EspTb6612Bridge::applyPhase(ledc_channel_t pwm_channel,
                                 gpio_num_t in1_gpio,
                                 gpio_num_t in2_gpio,
                                 int16_t signed_duty,
                                 bool invert,
                                 Direction& last_direction) noexcept
{
    int32_t duty = std::clamp<int32_t>(signed_duty,
                                       -static_cast<int32_t>(kMaximumDuty),
                                       static_cast<int32_t>(kMaximumDuty));
    if (invert) {
        duty = -duty;
    }

    Direction next_direction = Direction::stop;
    if (duty > 0) {
        next_direction = Direction::forward;
    } else if (duty < 0) {
        next_direction = Direction::reverse;
    }

    if (next_direction != last_direction) {
        if (!setDuty(pwm_channel, 0) ||
            !setDirection(in1_gpio, in2_gpio, next_direction)) {
            return false;
        }
        last_direction = next_direction;
    }

    const uint32_t magnitude = static_cast<uint32_t>(duty < 0 ? -duty : duty);
    return setDuty(pwm_channel, magnitude);
}

bool EspTb6612Bridge::setDirection(gpio_num_t in1_gpio,
                                   gpio_num_t in2_gpio,
                                   Direction direction) noexcept
{
    switch (direction) {
    case Direction::forward:
        return gpio_set_level(in2_gpio, 0) == ESP_OK &&
               gpio_set_level(in1_gpio, 1) == ESP_OK;
    case Direction::reverse:
        return gpio_set_level(in1_gpio, 0) == ESP_OK &&
               gpio_set_level(in2_gpio, 1) == ESP_OK;
    case Direction::stop:
    default:
        return gpio_set_level(in1_gpio, 0) == ESP_OK &&
               gpio_set_level(in2_gpio, 0) == ESP_OK;
    }
}

bool EspTb6612Bridge::setDuty(ledc_channel_t channel, uint32_t duty) noexcept
{
    return ledc_set_duty(kSpeedMode, channel, duty) == ESP_OK &&
           ledc_update_duty(kSpeedMode, channel) == ESP_OK;
}

} // namespace bsp
