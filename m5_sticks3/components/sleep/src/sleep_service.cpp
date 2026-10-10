#include "sleep_service.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <limits>
#include "app_manager.hpp"
#include "bsp_board.hpp"
#include "clock_service.hpp"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/task.h"

namespace sleep_service {
namespace {
constexpr char kTag[] = "sleep";
constexpr uint32_t kNoRequest = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kPublishingRequest = kNoRequest - 1;
constexpr uint32_t kMaxTimerSeconds = 86400;
constexpr uint64_t kWakeKeys = (1ULL << bsp::kKey1) | (1ULL << bsp::kKey2);
// L3B consumers must not feed their switched supply through signal pins.
constexpr std::array<gpio_num_t, 11> kPeripheralPins = {
    bsp::kLcdMosi, bsp::kLcdSclk, bsp::kLcdCs, bsp::kLcdDc,
    bsp::kLcdReset, bsp::kLcdBacklight, bsp::kAudioMclk,
    bsp::kAudioBclk, bsp::kAudioWs, bsp::kAudioDout, bsp::kAudioDin,
};
std::atomic<uint32_t> request{kNoRequest};
std::atomic<esp_err_t> error{ESP_OK};

esp_err_t isolate_peripherals()
{
    for (const auto pin : kPeripheralPins) {
        esp_err_t result;
        if (rtc_gpio_is_valid_gpio(pin)) {
            result = rtc_gpio_isolate(pin);
        } else {
            result = gpio_reset_pin(pin);
            if (result == ESP_OK) result = gpio_set_direction(pin, GPIO_MODE_DISABLE);
            if (result == ESP_OK) result = gpio_set_pull_mode(pin, GPIO_FLOATING);
            if (result == ESP_OK) result = gpio_hold_en(pin);
        }
        if (result != ESP_OK) return result;
    }
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}

esp_err_t fail(esp_err_t result)
{
    error.store(result);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGE(kTag, "sleep canceled: %s", esp_err_to_name(result));
    return result;
}
} // namespace

bool request_sleep(uint32_t timer_seconds)
{
    if (timer_seconds > kMaxTimerSeconds) return false;
    uint32_t expected = kNoRequest;
    const bool accepted = request.compare_exchange_strong(expected, kPublishingRequest);
    if (accepted) {
        error.store(ESP_OK);
        request.store(timer_seconds);
    }
    return accepted;
}

bool take_request(uint32_t& timer_seconds)
{
    uint32_t value = request.load();
    if (value > kMaxTimerSeconds || !request.compare_exchange_strong(value, kNoRequest)) return false;
    timer_seconds = value;
    return true;
}

esp_err_t last_error() { return error.load(); }

void restore_after_wake()
{
    // Clear the latched global state as well as the automatic sleep-hold bit.
    // This also restores pads following an aborted preparation and soft reset.
    gpio_force_unhold_all();
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(bsp::kInternalI2cSda);
    gpio_hold_dis(bsp::kInternalI2cScl);
    for (const auto pin : kPeripheralPins) {
        gpio_hold_dis(pin);
        if (rtc_gpio_is_valid_gpio(pin)) {
            rtc_gpio_hold_dis(pin);
            rtc_gpio_deinit(pin);
        }
    }
    for (const auto pin : {bsp::kKey1, bsp::kKey2}) {
        rtc_gpio_hold_dis(pin);
        rtc_gpio_deinit(pin);
    }
    const auto cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(kTag, "wake cause=%u keys=0x%llx epoch=%lld",
             static_cast<unsigned>(cause),
             static_cast<unsigned long long>(esp_sleep_get_ext1_wakeup_status()),
             static_cast<long long>(std::time(nullptr)));
}

esp_err_t enter_deep_sleep(app::AppManager& manager, uint32_t timer_seconds)
{
    if (timer_seconds > kMaxTimerSeconds || !manager.is_running())
        return fail(ESP_ERR_INVALID_ARG);
    // Confirm stable release after the UI's triggering long press. Otherwise an
    // active-low ext1 source would immediately reboot the device out of sleep.
    for (unsigned sample = 0; sample < 3; ++sample) {
        if (!gpio_get_level(bsp::kKey1) || !gpio_get_level(bsp::kKey2))
            return fail(ESP_ERR_INVALID_STATE);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_err_t result = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (result == ESP_OK) result = esp_sleep_enable_ext1_wakeup_io(kWakeKeys, ESP_EXT1_WAKEUP_ANY_LOW);
    if (result == ESP_OK && timer_seconds)
        result = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_seconds) * 1'000'000);
    // RTC_DATA_ATTR clock metadata uses IDF's automatic RTC memory retention.
    if (result != ESP_OK) return fail(result);

    const auto clock = clock_service::snapshot();
    clock_service::prepare_for_sleep();
    // Stop consumers before isolating pins or switching their rails off.
    const auto stopped = manager.deinitialize_all();
    if (!stopped) {
        ESP_LOGE(kTag, "module teardown failed: %.*s", static_cast<int>(stopped.module_name.size()),
                 stopped.module_name.data());
        return fail(ESP_FAIL);
    }
    auto& board = bsp::Board::instance();
    result = board.initialize();
    if (result == ESP_OK) result = isolate_peripherals();
    if (result == ESP_OK) result = board.prepare_l2_sleep();
    if (result != ESP_OK) return fail(result);

    ESP_LOGI(kTag, "L2 deep sleep: timer=%lu s keys=11,12 rtc_valid=%u epoch=%lld",
             static_cast<unsigned long>(timer_seconds), static_cast<unsigned>(clock.valid),
             static_cast<long long>(std::time(nullptr)));
    std::fflush(stdout);
    // Allow the final log to drain before native USB disconnects.
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
    return fail(ESP_FAIL);
}
} // namespace sleep_service
