#pragma once

/// cpp standard library headers
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

#include "led_strip_driver.hpp"

/**
 * @brief W2812B / WS2812B addressable LED strip controller (business layer).
 *
 * Owns a GRB pixel buffer and provides a rich API for colour management,
 * brightness control, effects, and HSV colour space conversions.
 * Pixel data is pushed to the hardware through the BSP ILedStripDriver.
 *
 * Colour format:
 *   - Internal storage: GRB (Green, Red, Blue) — the native WS2812B order.
 *   - Public API:       RGB (Red, Green, Blue) — converted internally.
 *
 * Typical usage:
 * @code
 *   // Create the hardware driver (BSP layer)
 *   Esp32RmtLedStripDriver::Config cfg;
 *   cfg.gpio_num = GPIO_NUM_8;
 *   cfg.max_leds = 16;
 *   auto driver = std::make_shared<Esp32RmtLedStripDriver>(cfg);
 *   driver->init();
 *   driver->start();
 *
 *   // Create the controller (business layer)
 *   Ws2812bStrip strip(driver, 16);
 *   strip.setBrightness(128);
 *   strip.setAll(255, 0, 0);      // all red at half brightness
 *   strip.show();
 * @endcode
 */
class Ws2812bStrip
{
public:
    // ====================================================================
    //  Colour types
    // ====================================================================

    /** 24-bit RGB colour (8 bits per channel). */
    struct Color {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        constexpr Color() = default;
        constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}

        bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
        bool operator!=(const Color& o) const { return !(*this == o); }

        /** Scale each channel by a 0-255 brightness factor. */
        Color dimmed(uint8_t brightness) const {
            return Color{
                static_cast<uint8_t>((static_cast<uint16_t>(r) * brightness) >> 8),
                static_cast<uint8_t>((static_cast<uint16_t>(g) * brightness) >> 8),
                static_cast<uint8_t>((static_cast<uint16_t>(b) * brightness) >> 8),
            };
        }
    };

    /** HSV colour (hue 0-359, saturation 0-255, value 0-255). */
    struct Hsv {
        uint16_t h = 0;    ///< Hue 0–359 degrees
        uint8_t  s = 255;  ///< Saturation 0–255
        uint8_t  v = 255;  ///< Value (brightness) 0–255

        constexpr Hsv() = default;
        constexpr Hsv(uint16_t h_, uint8_t s_, uint8_t v_) : h(h_), s(s_), v(v_) {}
    };

    // ====================================================================
    //  Construction
    // ====================================================================

    /**
     * @param driver     BSP driver (already initialised by caller, or will be)
     * @param num_pixels Number of LEDs in the strip.
     */
    Ws2812bStrip(std::shared_ptr<ILedStripDriver> driver, size_t num_pixels);
    ~Ws2812bStrip() = default;

    /// -------- lifecycle (delegates to driver) --------
    bool init()   { return driver_ ? driver_->init()  : false; }
    bool start()  { return driver_ ? driver_->start() : false; }
    bool stop()   { return driver_ ? driver_->stop()  : false; }

    /// -------- pixel manipulation --------

    /** Set a single pixel (RGB). */
    void setPixel(size_t index, uint8_t r, uint8_t g, uint8_t b);
    void setPixel(size_t index, const Color& c) { setPixel(index, c.r, c.g, c.b); }
    void setPixel(size_t index, const Hsv&   hsv);

    /** Fill the entire strip with one colour (RGB). */
    void setAll(uint8_t r, uint8_t g, uint8_t b);
    void setAll(const Color& c) { setAll(c.r, c.g, c.b); }
    void setAll(const Hsv&   hsv);

    /** Read a pixel (returns RGB, converted from internal GRB storage). */
    Color getPixel(size_t index) const;

    /** Turn off every pixel in the buffer (call show() afterwards). */
    void clear();

    /// -------- global brightness --------

    /**
     * @brief Set a global brightness scale (0–255).
     *
     * Brightness is applied when show() is called — the raw buffer holds
     * full-scale values; the dimming is done on-the-fly during transmission.
     * This allows brightness changes without re-calling setPixel/setAll.
     */
    void setBrightness(uint8_t level) { brightness_ = level; }
    uint8_t brightness() const { return brightness_; }

    /// -------- effects --------

    /** Fill with a smooth rainbow gradient across the strip. */
    void fillRainbow(uint8_t initial_hue = 0, uint8_t delta_hue = 0);

    /** Fill the strip with a chasing-pixel pattern. */
    void fillChase(const Color& a, const Color& b, size_t width, size_t offset = 0);

    /**
     * @brief Create a colour gradient between two colours.
     *
     * Blends @p c1 at pixel 0 to @p c2 at pixel num_pixels-1.
     */
    void fillGradient(const Color& c1, const Color& c2);

    /** Set a single pixel and immediately call show() (convenience for sparse updates). */
    void setPixelAndShow(size_t index, uint8_t r, uint8_t g, uint8_t b);

    /// -------- transmission --------

    /**
     * @brief Push the pixel buffer to the LED strip hardware.
     *
     * Applies the global brightness during encoding, then calls
     * driver_->refresh().
     */
    bool show();

    /// -------- helpers --------

    /** @return Number of pixels in the strip. */
    size_t size() const { return num_pixels_; }

    /** Direct access to the internal GRB buffer for custom effects. */
    uint8_t*       rawBuffer()       { return pixel_buf_.data(); }
    const uint8_t* rawBuffer() const { return pixel_buf_.data(); }

    /** Access the underlying BSP driver. */
    std::shared_ptr<ILedStripDriver> driver() const { return driver_; }

    // ====================================================================
    //  Static colour utilities
    // ====================================================================

    /** Convert HSV → RGB (all channels 0–255). */
    static Color hsvToRgb(const Hsv& hsv);

    /** Convert RGB → HSV. */
    static Hsv   rgbToHsv(const Color& c);

    /**
     * @brief Generate a rainbow colour for a given wheel position.
     *
     * @param wheel_pos 0–255 maps across the full hue wheel.
     * @return RGB colour.
     */
    static Color wheel(uint8_t wheel_pos);

    /** Linear interpolation between two colours.  t ∈ [0, 255]. */
    static Color lerp(const Color& a, const Color& b, uint8_t t);

private:
    std::shared_ptr<ILedStripDriver> driver_;
    size_t                           num_pixels_;
    std::vector<uint8_t>             pixel_buf_;   ///< Internal GRB buffer
    uint8_t                          brightness_ = 255;
};
