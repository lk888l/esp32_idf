#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace bsp {

// StickS3 uses the ST7789P3 panel in its native portrait orientation.
constexpr uint32_t kDisplayWidth = 135;
constexpr uint32_t kDisplayHeight = 240;

class Display {
public:
    static Display& instance();

    esp_err_t initialize();
    esp_err_t deinitialize();
    esp_err_t fade_backlight(uint8_t percent, uint32_t duration_ms);

    bool initialized() const { return initialized_; }
    lv_display_t* lv_display() const { return lv_display_; }

private:
    struct PerformanceStats {
        int64_t window_started_us = 0;
        int64_t render_started_us = 0;
        int64_t flush_started_us = 0;
        uint64_t render_time_us = 0;
        uint64_t flush_time_us = 0;
        uint32_t rendered_frames = 0;
        uint32_t flushes = 0;
    };

    Display() = default;
    esp_err_t initialize_resources();
    static void performance_event_callback(lv_event_t* event);
    void record_performance_event(lv_event_code_t code);

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* lv_display_ = nullptr;
    bool backlight_timer_initialized_ = false;
    bool backlight_channel_initialized_ = false;
    bool spi_initialized_ = false;
    bool lvgl_initialized_ = false;
    bool fade_initialized_ = false;
    bool initialized_ = false;
    PerformanceStats performance_{};
};

} // namespace bsp
