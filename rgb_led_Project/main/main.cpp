// main.cpp — W2812B rainbow gradient demo
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// BSP layer
#include "uart_dma_driver.hpp"
#include "rmt_led_strip_driver.hpp"

// Business layer
#include "ws2812b.hpp"

// Project
#include "logger.hpp"

// cpp standard library
#include <memory>

static const char* TAG = "app_main";

/// ──────────── LED strip configuration ────────────
static constexpr int      LED_GPIO       = GPIO_NUM_27;   ///< W2812B data pin
static constexpr size_t   LED_COUNT      = 16;           ///< Number of LEDs
static constexpr uint8_t  BRIGHTNESS     = 64;           ///< Global brightness (0-255)
static constexpr uint8_t  RAINBOW_SPEED  = 2;            ///< Hue advance per frame

extern "C" void app_main(void)
{
    // ── 1. UART DMA (logging) ───────────────────────────────────
    Esp32UartDmaDriver::Config uart_cfg = {};
    uart_cfg.uart_num = UART_NUM_0;
    uart_cfg.tx_pin   = GPIO_NUM_11;
    uart_cfg.rx_pin   = GPIO_NUM_12;
    uart_cfg.baudrate = 115200;
    Esp32UartDmaDriver uart_drv(uart_cfg);

    if (uart_drv.init() && uart_drv.start()) {
        espidf_template::Logger::setUart(&uart_drv);
        esp_log_set_vprintf(&espidf_template::Logger::vprintfHook);
    }

    espidf_template::Logger logger(TAG);
    logger.info("W2812B rainbow demo starting...");

    // ── 2. LED strip — BSP driver ───────────────────────────────
    Esp32RmtLedStripDriver::Config led_cfg = {};
    led_cfg.gpio_num = LED_GPIO;
    led_cfg.max_leds = LED_COUNT;
    auto driver = std::make_shared<Esp32RmtLedStripDriver>(led_cfg);

    if (!driver->init()) {
        logger.error("RMT LED driver init failed");
        return;
    }
    if (!driver->start()) {
        logger.error("RMT LED driver start failed");
        return;
    }

    // ── 3. LED strip — business controller ──────────────────────
    Ws2812bStrip strip(driver, LED_COUNT);
    strip.setBrightness(BRIGHTNESS);
    strip.clear();
    strip.show();

    logger.info("LED strip ready — {} pixels on GPIO {}", LED_COUNT, LED_GPIO);

    // ── 4. Rainbow loop ─────────────────────────────────────────
    uint8_t hue = 0;
    while (true) {
        strip.fillRainbow(hue, 255 / LED_COUNT);  // spread full hue range across strip
        strip.show();

        hue += RAINBOW_SPEED;
        vTaskDelay(pdMS_TO_TICKS(30));            // ~33 fps
    }
}
