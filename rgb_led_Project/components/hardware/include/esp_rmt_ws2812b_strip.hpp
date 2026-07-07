#pragma once

#include "ws2812b_strip.hpp"

#include <vector>

#include "driver/rmt_types.h"

namespace hardware {

class EspRmtWs2812bStrip final : public IWs2812bStrip {
public:
    explicit EspRmtWs2812bStrip(Ws2812bConfig config);
    ~EspRmtWs2812bStrip() override;

    bool initialize() override;
    void deinitialize() override;

    void setPixel(std::size_t index, Rgb color) override;
    void clear() override;
    bool show() override;

    std::size_t size() const override { return config_.led_count; }
    void setBrightness(uint8_t brightness) override { config_.brightness = brightness; }
    uint8_t brightness() const override { return config_.brightness; }

private:
    static uint8_t scale(uint8_t value, uint8_t brightness);

    Ws2812bConfig config_;
    std::vector<uint8_t> pixels_;
    std::vector<rmt_symbol_word_t> symbols_;
    rmt_channel_handle_t channel_ = nullptr;
    rmt_encoder_handle_t encoder_ = nullptr;
};

} // namespace hardware
