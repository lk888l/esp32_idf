#include "bsp_display.hpp"

#include "bsp_board.hpp"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

namespace bsp {
namespace {

constexpr char kTag[] = "display";
constexpr spi_host_device_t kLcdSpiHost = SPI3_HOST;
constexpr uint32_t kLcdPixelClockHz = 40 * 1000 * 1000;
constexpr uint32_t kDrawBufferLines = 60;
constexpr ledc_mode_t kBacklightMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr uint32_t kBacklightMax = (1U << 10) - 1;

} // namespace

Display& Display::instance()
{
    static Display display;
    return display;
}

esp_err_t Display::initialize()
{
    if (initialized_) {
        return ESP_OK;
    }

    const esp_err_t result = initialize_resources();
    if (result != ESP_OK) {
        deinitialize();
    }
    return result;
}

esp_err_t Display::initialize_resources()
{
    ESP_RETURN_ON_FALSE(Board::instance().initialized(), ESP_ERR_INVALID_STATE, kTag,
                        "board must be initialized first");
    ESP_RETURN_ON_ERROR(Board::instance().enable_display_power(), kTag,
                        "failed to enable LCD power rail");

    const ledc_timer_config_t timer_config = {
        .speed_mode = kBacklightMode,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = kBacklightTimer,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "backlight timer setup failed");
    backlight_timer_initialized_ = true;
    const ledc_channel_config_t channel_config = {
        .gpio_num = kLcdBacklight,
        .speed_mode = kBacklightMode,
        .channel = kBacklightChannel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = kBacklightTimer,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0,
        },
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag, "backlight channel setup failed");
    backlight_channel_initialized_ = true;
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), kTag, "backlight fade setup failed");
    fade_initialized_ = true;

    const spi_bus_config_t bus_config = {
        .mosi_io_num = kLcdMosi,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = kLcdSclk,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .data_io_default_level = false,
        .max_transfer_sz = static_cast<int>(kDisplayWidth * kDrawBufferLines * sizeof(uint16_t) + 8),
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kLcdSpiHost, &bus_config, SPI_DMA_CH_AUTO), kTag,
                        "LCD SPI bus setup failed");
    spi_initialized_ = true;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = kLcdCs,
        .dc_gpio_num = kLcdDc,
        .spi_mode = 0,
        .pclk_hz = kLcdPixelClockHz,
        .trans_queue_depth = 10,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 0,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost),
                                 &io_config, &io_),
        kTag, "LCD panel IO setup failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = kLcdReset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = nullptr,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_, &panel_config, &panel_), kTag,
                        "ST7789 panel setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), kTag, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_), kTag, "LCD init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_, true), kTag, "LCD invert setup failed");
    // Native 135x240 scan direction. The controller RAM is 240x320, so this
    // module is mounted at X=52, Y=40 inside the ST7789 address space.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_, false), kTag, "LCD rotate setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_, false, false), kTag, "LCD mirror setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel_, 52, 40), kTag, "LCD window setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_, true), kTag, "LCD enable failed");

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_config.task_priority = 4;
    lvgl_config.task_stack = 8192;
    lvgl_config.task_affinity = 0;
    lvgl_config.task_max_sleep_ms = 20;
    lvgl_config.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), kTag, "LVGL port setup failed");
    lvgl_initialized_ = true;

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = kDisplayWidth * kDrawBufferLines,
        .double_buffer = true,
        .trans_size = 0,
        .hres = kDisplayWidth,
        .vres = kDisplayHeight,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .rounder_cb = nullptr,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    lv_display_ = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(lv_display_, ESP_ERR_NO_MEM, kTag, "failed to attach display to LVGL");
    performance_ = {};
    lv_display_add_event_cb(lv_display_, performance_event_callback, LV_EVENT_ALL, this);

    initialized_ = true;
    ESP_LOGI(kTag, "ST7789P3 ready: %lux%lu, %luMHz SPI, 2x%lu-line DMA buffer",
             kDisplayWidth, kDisplayHeight, kLcdPixelClockHz / 1000000,
             kDrawBufferLines);
    return ESP_OK;
}

void Display::performance_event_callback(lv_event_t* event)
{
    auto* display = static_cast<Display*>(lv_event_get_user_data(event));
    display->record_performance_event(lv_event_get_code(event));
}

void Display::record_performance_event(lv_event_code_t code)
{
    const int64_t now = esp_timer_get_time();
    if (performance_.window_started_us == 0) {
        performance_.window_started_us = now;
    }

    switch (code) {
    case LV_EVENT_RENDER_START:
        performance_.render_started_us = now;
        break;
    case LV_EVENT_RENDER_READY:
        if (performance_.render_started_us != 0) {
            performance_.render_time_us += now - performance_.render_started_us;
            performance_.render_started_us = 0;
        }
        ++performance_.rendered_frames;
        break;
    case LV_EVENT_FLUSH_START:
        performance_.flush_started_us = now;
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (performance_.flush_started_us != 0) {
            performance_.flush_time_us += now - performance_.flush_started_us;
            performance_.flush_started_us = 0;
        }
        ++performance_.flushes;
        break;
    default:
        return;
    }

    const int64_t elapsed_us = now - performance_.window_started_us;
    if (elapsed_us < 2000000 || performance_.rendered_frames == 0) {
        return;
    }

    const uint32_t fps_tenths = static_cast<uint32_t>(
        performance_.rendered_frames * 10000000ULL / elapsed_us);
    const uint32_t render_us = static_cast<uint32_t>(
        performance_.render_time_us / performance_.rendered_frames);
    const uint32_t flush_us = static_cast<uint32_t>(
        performance_.flush_time_us / performance_.rendered_frames);
    const uint32_t chunks_tenths = performance_.flushes * 10U /
                                      performance_.rendered_frames;
    ESP_LOGI(kTag,
             "perf %lu.%lu fps render=%lu.%03lums flush=%lu.%03lums chunks=%lu.%lu/frame",
             fps_tenths / 10, fps_tenths % 10,
             render_us / 1000, render_us % 1000,
             flush_us / 1000, flush_us % 1000,
             chunks_tenths / 10, chunks_tenths % 10);
    performance_ = {.window_started_us = now};
}

esp_err_t Display::fade_backlight(uint8_t percent, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(fade_initialized_, ESP_ERR_INVALID_STATE, kTag,
                        "backlight is not initialized");
    if (percent > 100) {
        percent = 100;
    }
    const uint32_t duty = (kBacklightMax * percent) / 100;
    ESP_RETURN_ON_ERROR(
        ledc_set_fade_with_time(kBacklightMode, kBacklightChannel, duty, duration_ms),
        kTag, "failed to prepare backlight fade");
    return ledc_fade_start(kBacklightMode, kBacklightChannel, LEDC_FADE_NO_WAIT);
}

esp_err_t Display::deinitialize()
{
    if (!initialized_ && !spi_initialized_ && !lvgl_initialized_ && !fade_initialized_ &&
        !backlight_channel_initialized_ && !backlight_timer_initialized_ && !lv_display_ &&
        !panel_ && !io_) {
        return ESP_OK;
    }
    initialized_ = false;

    if (fade_initialized_) {
        ledc_set_duty(kBacklightMode, kBacklightChannel, 0);
        ledc_update_duty(kBacklightMode, kBacklightChannel);
    }
    if (lv_display_) {
        ESP_RETURN_ON_ERROR(lvgl_port_remove_disp(lv_display_), kTag,
                            "failed to remove LVGL display");
        lv_display_ = nullptr;
    }
    if (lvgl_initialized_) {
        ESP_RETURN_ON_ERROR(lvgl_port_deinit(), kTag, "failed to deinitialize LVGL port");
        lvgl_initialized_ = false;
    }
    if (panel_) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_del(panel_), kTag, "failed to delete LCD panel");
        panel_ = nullptr;
    }
    if (io_) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(io_), kTag, "failed to delete LCD panel IO");
        io_ = nullptr;
    }
    if (spi_initialized_) {
        ESP_RETURN_ON_ERROR(spi_bus_free(kLcdSpiHost), kTag, "failed to release LCD SPI bus");
        spi_initialized_ = false;
    }
    if (fade_initialized_) {
        ledc_fade_func_uninstall();
        fade_initialized_ = false;
    }
    if (backlight_channel_initialized_) {
        ESP_RETURN_ON_ERROR(ledc_stop(kBacklightMode, kBacklightChannel, 0), kTag,
                            "failed to stop backlight channel");
        ESP_RETURN_ON_ERROR(gpio_reset_pin(kLcdBacklight), kTag,
                            "failed to reset backlight GPIO");
        backlight_channel_initialized_ = false;
    }
    if (backlight_timer_initialized_) {
        ESP_RETURN_ON_ERROR(ledc_timer_pause(kBacklightMode, kBacklightTimer), kTag,
                            "failed to pause backlight timer");
        const ledc_timer_config_t timer_config = {
            .speed_mode = kBacklightMode,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = kBacklightTimer,
            .freq_hz = 0,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = true,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag,
                            "failed to release backlight timer");
        backlight_timer_initialized_ = false;
    }
    return ESP_OK;
}

} // namespace bsp
