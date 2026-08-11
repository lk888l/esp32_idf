#include "wave_generator.hpp"

#include "bsp_board.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

namespace wave {
namespace {

constexpr char kTag[] = "wave_generator";
constexpr ledc_mode_t kWaveMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kWaveTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kWaveChannelG4 = LEDC_CHANNEL_1;
constexpr ledc_channel_t kWaveChannelG5 = LEDC_CHANNEL_2;
constexpr gpio_num_t kWaveOutputG4 = GPIO_NUM_4;
constexpr gpio_num_t kWaveOutputG5 = GPIO_NUM_5;

} // namespace

Generator& Generator::instance()
{
    static Generator generator;
    return generator;
}

esp_err_t Generator::initialize()
{
    if (initialized_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(bsp::Board::instance().initialized(), ESP_ERR_INVALID_STATE, kTag,
                        "board must be initialized first");

    const esp_err_t power_result = bsp::Board::instance().enable_5v_output();
    if (power_result != ESP_OK) {
        ESP_LOGW(kTag, "external 5 V output unavailable: %s",
                 esp_err_to_name(power_result));
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = kWaveMode,
        .duty_resolution = LEDC_TIMER_2_BIT,
        .timer_num = kWaveTimer,
        .freq_hz = frequency_hz_,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    esp_err_t result = ledc_timer_config(&timer_config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "timer configuration failed: %s", esp_err_to_name(result));
        return result;
    }
    timer_initialized_ = true;

    result = configure_channel(kWaveOutputG4, kWaveChannelG4, 0);
    if (result != ESP_OK) {
        deinitialize();
        return result;
    }
    channel_g4_initialized_ = true;

    result = configure_channel(kWaveOutputG5, kWaveChannelG5, 0);
    if (result != ESP_OK) {
        deinitialize();
        return result;
    }
    channel_g5_initialized_ = true;

    const uint32_t actual_hz = ledc_get_freq(kWaveMode, kWaveTimer);
    const uint32_t duty_g4 = ledc_get_duty(kWaveMode, kWaveChannelG4);
    const uint32_t duty_g5 = ledc_get_duty(kWaveMode, kWaveChannelG5);
    if (actual_hz == 0 || duty_g4 != 0 || duty_g5 != 0) {
        ESP_LOGE(kTag, "LEDC readback failed: frequency=%lu duty_g4=%lu duty_g5=%lu",
                 actual_hz, duty_g4, duty_g5);
        deinitialize();
        return ESP_FAIL;
    }

    // Exercise every UI preset while both channels remain at zero duty. This
    // catches unsupported timer dividers at boot without emitting a pulse.
    for (const uint32_t requested_hz : kFrequencyPresetsHz) {
        const esp_err_t result =
            ledc_set_freq(kWaveMode, kWaveTimer, requested_hz);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "LEDC preset self-test failed at %lu Hz: %s",
                     requested_hz, esp_err_to_name(result));
            deinitialize();
            return result;
        }
        const uint32_t preset_actual_hz = ledc_get_freq(kWaveMode, kWaveTimer);
        if (preset_actual_hz == 0) {
            ESP_LOGE(kTag, "LEDC preset readback failed at %lu Hz", requested_hz);
            deinitialize();
            return ESP_FAIL;
        }
    }

    const esp_err_t restore_result =
        ledc_set_freq(kWaveMode, kWaveTimer, frequency_hz_);
    const uint32_t restored_hz = ledc_get_freq(kWaveMode, kWaveTimer);
    if (restore_result != ESP_OK || restored_hz == 0 ||
        ledc_get_duty(kWaveMode, kWaveChannelG4) != 0 ||
        ledc_get_duty(kWaveMode, kWaveChannelG5) != 0) {
        ESP_LOGE(kTag, "LEDC failed to restore the muted default preset");
        deinitialize();
        return ESP_FAIL;
    }

    frequency_hz_ = restored_hz;
    initialized_ = true;
    enabled_ = false;
    ESP_LOGI(kTag, "%u presets verified; G4/G5 ready at %lu Hz (muted, duty=0/4)",
             static_cast<unsigned>(kFrequencyPresetsHz.size()), restored_hz);
    return ESP_OK;
}

esp_err_t Generator::deinitialize()
{
    esp_err_t first_error = ESP_OK;
    if (channel_g4_initialized_) {
        const esp_err_t result = ledc_stop(kWaveMode, kWaveChannelG4, 0);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
        gpio_reset_pin(kWaveOutputG4);
        channel_g4_initialized_ = false;
    }
    if (channel_g5_initialized_) {
        const esp_err_t result = ledc_stop(kWaveMode, kWaveChannelG5, 0);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
        gpio_reset_pin(kWaveOutputG5);
        channel_g5_initialized_ = false;
    }
    if (timer_initialized_) {
        const esp_err_t pause_result = ledc_timer_pause(kWaveMode, kWaveTimer);
        if (first_error == ESP_OK && pause_result != ESP_OK) {
            first_error = pause_result;
        }
        const ledc_timer_config_t timer_config = {
            .speed_mode = kWaveMode,
            .duty_resolution = LEDC_TIMER_2_BIT,
            .timer_num = kWaveTimer,
            .freq_hz = 0,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = true,
        };
        const esp_err_t result = ledc_timer_config(&timer_config);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
        timer_initialized_ = false;
    }

    initialized_ = false;
    enabled_ = false;
    return first_error;
}

esp_err_t Generator::set_frequency(uint32_t frequency_hz)
{
    ESP_RETURN_ON_FALSE(initialized_, ESP_ERR_INVALID_STATE, kTag,
                        "generator is not initialized");
    ESP_RETURN_ON_FALSE(frequency_hz > 0, ESP_ERR_INVALID_ARG, kTag,
                        "frequency must be non-zero");

    ESP_RETURN_ON_ERROR(ledc_set_freq(kWaveMode, kWaveTimer, frequency_hz), kTag,
                        "unable to set frequency to %lu Hz", frequency_hz);
    const uint32_t actual_hz = ledc_get_freq(kWaveMode, kWaveTimer);
    ESP_RETURN_ON_FALSE(actual_hz > 0, ESP_FAIL, kTag,
                        "unable to read back frequency after requesting %lu Hz", frequency_hz);

    frequency_hz_ = actual_hz;
    if (enabled_) {
        ESP_RETURN_ON_ERROR(set_duty(duty_ticks_), kTag,
                            "failed to restore duty after frequency change");
    }
    ESP_LOGI(kTag, "frequency requested=%lu Hz actual=%lu Hz", frequency_hz, actual_hz);
    return ESP_OK;
}

esp_err_t Generator::set_duty_percent(uint8_t duty_percent)
{
    ESP_RETURN_ON_FALSE(initialized_, ESP_ERR_INVALID_STATE, kTag,
                        "generator is not initialized");
    ESP_RETURN_ON_FALSE(duty_percent == 25 || duty_percent == 50 || duty_percent == 75,
                        ESP_ERR_INVALID_ARG, kTag,
                        "duty must be 25, 50, or 75 percent");

    const uint32_t requested_ticks = duty_percent / 25;
    if (enabled_) {
        const esp_err_t result = set_duty(requested_ticks);
        if (result != ESP_OK) {
            enabled_ = false;
            return result;
        }
        const uint32_t duty_g4 = ledc_get_duty(kWaveMode, kWaveChannelG4);
        const uint32_t duty_g5 = ledc_get_duty(kWaveMode, kWaveChannelG5);
        if (duty_g4 != requested_ticks || duty_g5 != requested_ticks) {
            ESP_LOGE(kTag, "duty readback failed: requested=%lu G4=%lu G5=%lu",
                     requested_ticks, duty_g4, duty_g5);
            set_duty(0);
            enabled_ = false;
            return ESP_FAIL;
        }
    }

    duty_percent_ = duty_percent;
    duty_ticks_ = requested_ticks;
    ESP_LOGI(kTag, "duty set to %u%% (%lu/4)%s", duty_percent_,
             duty_ticks_, enabled_ ? "" : " while muted");
    return ESP_OK;
}

esp_err_t Generator::set_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(initialized_, ESP_ERR_INVALID_STATE, kTag,
                        "generator is not initialized");
    if (enabled_ == enabled) {
        return ESP_OK;
    }

    const uint32_t requested_ticks = enabled ? duty_ticks_ : 0;
    const esp_err_t result = set_duty(requested_ticks);
    if (result != ESP_OK) {
        enabled_ = false;
        return result;
    }
    const uint32_t duty_g4 = ledc_get_duty(kWaveMode, kWaveChannelG4);
    const uint32_t duty_g5 = ledc_get_duty(kWaveMode, kWaveChannelG5);
    if (duty_g4 != requested_ticks || duty_g5 != requested_ticks) {
        set_duty(0);
        enabled_ = false;
        ESP_LOGE(kTag, "output state readback failed: expected=%lu G4=%lu G5=%lu",
                 requested_ticks, duty_g4, duty_g5);
        return ESP_FAIL;
    }
    enabled_ = enabled;
    ESP_LOGI(kTag, "outputs %s at %lu Hz, duty=%u%%",
             enabled ? "enabled" : "muted", frequency_hz_, duty_percent_);
    return ESP_OK;
}

esp_err_t Generator::configure_channel(int gpio, int channel, uint32_t duty)
{
    const ledc_channel_config_t channel_config = {
        .gpio_num = gpio,
        .speed_mode = kWaveMode,
        .channel = static_cast<ledc_channel_t>(channel),
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = kWaveTimer,
        .duty = duty,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {.output_invert = 0},
    };
    return ledc_channel_config(&channel_config);
}

esp_err_t Generator::set_duty(uint32_t duty)
{
    ESP_RETURN_ON_ERROR(
        ledc_set_duty_and_update(kWaveMode, kWaveChannelG4, duty, 0), kTag,
        "failed to update G4 duty");
    const esp_err_t result =
        ledc_set_duty_and_update(kWaveMode, kWaveChannelG5, duty, 0);
    if (result != ESP_OK) {
        // Keep both outputs in a known safe state if only one channel updates.
        ledc_set_duty_and_update(kWaveMode, kWaveChannelG4, 0, 0);
        ledc_set_duty_and_update(kWaveMode, kWaveChannelG5, 0, 0);
        return result;
    }
    return ESP_OK;
}

} // namespace wave
