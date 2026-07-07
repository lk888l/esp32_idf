#include "esp_rmt_ws2812b_strip.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace hardware {
namespace {

constexpr uint32_t kRmtResolutionHz = 10'000'000;
constexpr uint16_t kT0hTicks = 4;
constexpr uint16_t kT0lTicks = 9;
constexpr uint16_t kT1hTicks = 8;
constexpr uint16_t kT1lTicks = 5;
constexpr uint16_t kResetTicks = 800;

rmt_symbol_word_t makeSymbol(uint16_t high_ticks, uint16_t low_ticks)
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = high_ticks;
    symbol.level0 = 1;
    symbol.duration1 = low_ticks;
    symbol.level1 = 0;
    return symbol;
}

rmt_symbol_word_t makeResetSymbol()
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = kResetTicks;
    symbol.level0 = 0;
    symbol.duration1 = 0;
    symbol.level1 = 0;
    return symbol;
}

} // namespace

EspRmtWs2812bStrip::EspRmtWs2812bStrip(Ws2812bConfig config)
    : config_(config)
    , pixels_(config.led_count * 3)
    , symbols_(config.led_count * 3 * 8 + 1)
{
}

EspRmtWs2812bStrip::~EspRmtWs2812bStrip()
{
    deinitialize();
}

bool EspRmtWs2812bStrip::initialize()
{
    if (channel_ != nullptr) {
        return true;
    }

    if (config_.data_pin < 0 || config_.led_count == 0) {
        return false;
    }

    rmt_tx_channel_config_t channel_config{};
    channel_config.gpio_num = static_cast<gpio_num_t>(config_.data_pin);
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = kRmtResolutionHz;
    channel_config.mem_block_symbols = 64;
    channel_config.trans_queue_depth = 4;

    if (rmt_new_tx_channel(&channel_config, &channel_) != ESP_OK) {
        return false;
    }

    rmt_copy_encoder_config_t encoder_config{};
    if (rmt_new_copy_encoder(&encoder_config, &encoder_) != ESP_OK) {
        deinitialize();
        return false;
    }

    if (rmt_enable(channel_) != ESP_OK) {
        deinitialize();
        return false;
    }

    return true;
}

void EspRmtWs2812bStrip::deinitialize()
{
    if (channel_ != nullptr) {
        rmt_disable(channel_);
    }

    if (encoder_ != nullptr) {
        rmt_del_encoder(encoder_);
        encoder_ = nullptr;
    }

    if (channel_ != nullptr) {
        rmt_del_channel(channel_);
        channel_ = nullptr;
    }
}

void EspRmtWs2812bStrip::setPixel(std::size_t index, Rgb color)
{
    if (index >= config_.led_count) {
        return;
    }

    const std::size_t offset = index * 3;
    pixels_[offset] = scale(color.g, config_.brightness);
    pixels_[offset + 1] = scale(color.r, config_.brightness);
    pixels_[offset + 2] = scale(color.b, config_.brightness);
}

void EspRmtWs2812bStrip::clear()
{
    std::fill(pixels_.begin(), pixels_.end(), 0);
}

bool EspRmtWs2812bStrip::show()
{
    if (channel_ == nullptr || encoder_ == nullptr || pixels_.empty()) {
        return false;
    }

    const rmt_symbol_word_t bit0 = makeSymbol(kT0hTicks, kT0lTicks);
    const rmt_symbol_word_t bit1 = makeSymbol(kT1hTicks, kT1lTicks);

    std::size_t symbol_index = 0;
    for (const uint8_t byte : pixels_) {
        for (int bit = 7; bit >= 0; --bit) {
            symbols_[symbol_index++] = ((byte >> bit) & 0x01) ? bit1 : bit0;
        }
    }

    symbols_[symbol_index++] = makeResetSymbol();

    rmt_transmit_config_t tx_config{};
    tx_config.loop_count = 0;

    const esp_err_t transmit_err = rmt_transmit(
        channel_,
        encoder_,
        symbols_.data(),
        symbol_index * sizeof(rmt_symbol_word_t),
        &tx_config);

    if (transmit_err != ESP_OK) {
        return false;
    }

    return rmt_tx_wait_all_done(channel_, pdMS_TO_TICKS(100)) == ESP_OK;
}

uint8_t EspRmtWs2812bStrip::scale(uint8_t value, uint8_t brightness)
{
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * brightness) / 255);
}

} // namespace hardware
