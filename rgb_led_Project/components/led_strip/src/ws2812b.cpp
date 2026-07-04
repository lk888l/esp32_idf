#include "ws2812b.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================================
//  Construction
// ============================================================================

Ws2812bStrip::Ws2812bStrip(std::shared_ptr<ILedStripDriver> driver, size_t num_pixels)
    : driver_(std::move(driver))
    , num_pixels_(num_pixels)
    , pixel_buf_(num_pixels * 3, 0)   // GRB, zero-initialised
{
}

// ============================================================================
//  Pixel manipulation — public API uses RGB, internal storage uses GRB
// ============================================================================

void Ws2812bStrip::setPixel(size_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= num_pixels_) return;

    const size_t offset = index * 3;
    pixel_buf_[offset + 0] = g;   // GRB order
    pixel_buf_[offset + 1] = r;
    pixel_buf_[offset + 2] = b;
}

void Ws2812bStrip::setPixel(size_t index, const Hsv& hsv)
{
    setPixel(index, hsvToRgb(hsv));
}

void Ws2812bStrip::setAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i < num_pixels_; ++i) {
        const size_t offset = i * 3;
        pixel_buf_[offset + 0] = g;
        pixel_buf_[offset + 1] = r;
        pixel_buf_[offset + 2] = b;
    }
}

void Ws2812bStrip::setAll(const Hsv& hsv)
{
    setAll(hsvToRgb(hsv));
}

Ws2812bStrip::Color Ws2812bStrip::getPixel(size_t index) const
{
    if (index >= num_pixels_) return {};

    const size_t offset = index * 3;
    return Color{
        pixel_buf_[offset + 1],   // R
        pixel_buf_[offset + 0],   // G
        pixel_buf_[offset + 2],   // B
    };
}

void Ws2812bStrip::clear()
{
    std::memset(pixel_buf_.data(), 0, pixel_buf_.size());
}

// ============================================================================
//  Transmission — applies global brightness on the fly
// ============================================================================

bool Ws2812bStrip::show()
{
    if (!driver_) return false;

    // Fast path: brightness == 255 → send raw buffer directly.
    if (brightness_ == 255) {
        return driver_->refresh(pixel_buf_.data(), num_pixels_);
    }

    // Dimmed path: scale each channel and send from a temporary buffer.
    std::vector<uint8_t> dimmed(num_pixels_ * 3);
    for (size_t i = 0; i < dimmed.size(); ++i) {
        dimmed[i] = static_cast<uint8_t>(
            (static_cast<uint16_t>(pixel_buf_[i]) * brightness_) >> 8
        );
    }
    return driver_->refresh(dimmed.data(), num_pixels_);
}

void Ws2812bStrip::setPixelAndShow(size_t index, uint8_t r, uint8_t g, uint8_t b)
{
    setPixel(index, r, g, b);
    show();
}

// ============================================================================
//  Effects
// ============================================================================

void Ws2812bStrip::fillRainbow(uint8_t initial_hue, uint8_t delta_hue)
{
    for (size_t i = 0; i < num_pixels_; ++i) {
        uint8_t hue = initial_hue + static_cast<uint8_t>(
            (static_cast<uint16_t>(i) * delta_hue)
        );
        setPixel(i, wheel(hue));
    }
}

void Ws2812bStrip::fillChase(const Color& a, const Color& b, size_t width, size_t offset)
{
    if (width == 0) width = 1;
    for (size_t i = 0; i < num_pixels_; ++i) {
        size_t phase = (i + offset) / width;
        setPixel(i, (phase % 2 == 0) ? a : b);
    }
}

void Ws2812bStrip::fillGradient(const Color& c1, const Color& c2)
{
    if (num_pixels_ <= 1) {
        if (num_pixels_ == 1) setPixel(0, c1);
        return;
    }

    for (size_t i = 0; i < num_pixels_; ++i) {
        uint8_t t = static_cast<uint8_t>((i * 255) / (num_pixels_ - 1));
        setPixel(i, lerp(c1, c2, t));
    }
}

// ============================================================================
//  Static: HSV ↔ RGB conversion
// ============================================================================

Ws2812bStrip::Color Ws2812bStrip::hsvToRgb(const Hsv& hsv)
{
    // Fast path: saturation == 0 → greyscale
    if (hsv.s == 0) {
        return Color{hsv.v, hsv.v, hsv.v};
    }

    // Hue is 0–359; map to 0–1529 (256 * 6 - 1) for the sector calculation
    const uint16_t sector_f = (static_cast<uint32_t>(hsv.h % 360) * 256U) / 60U;  // 0..1535
    const uint8_t  sector   = static_cast<uint8_t>(sector_f >> 8);   // 0..5
    const uint8_t  frac     = static_cast<uint8_t>(sector_f & 0xFF); // 0..255

    const uint16_t p = static_cast<uint16_t>(hsv.v) * (255 - hsv.s) / 255;
    const uint16_t q = static_cast<uint16_t>(hsv.v) * (255 - (static_cast<uint16_t>(hsv.s) * frac / 255)) / 255;
    const uint16_t t = static_cast<uint16_t>(hsv.v) * (255 - (static_cast<uint16_t>(hsv.s) * (255 - frac) / 255)) / 255;

    switch (sector) {
    case 0:  return Color{hsv.v, static_cast<uint8_t>(t), static_cast<uint8_t>(p)};
    case 1:  return Color{static_cast<uint8_t>(q), hsv.v, static_cast<uint8_t>(p)};
    case 2:  return Color{static_cast<uint8_t>(p), hsv.v, static_cast<uint8_t>(t)};
    case 3:  return Color{static_cast<uint8_t>(p), static_cast<uint8_t>(q), hsv.v};
    case 4:  return Color{static_cast<uint8_t>(t), static_cast<uint8_t>(p), hsv.v};
    default: return Color{hsv.v, static_cast<uint8_t>(p), static_cast<uint8_t>(q)};
    }
}

Ws2812bStrip::Hsv Ws2812bStrip::rgbToHsv(const Color& c)
{
    const uint8_t mx = std::max({c.r, c.g, c.b});
    const uint8_t mn = std::min({c.r, c.g, c.b});
    const uint8_t delta = mx - mn;

    Hsv result;
    result.v = mx;

    if (mx == 0 || delta == 0) {
        result.s = 0;
        result.h = 0;
        return result;
    }

    result.s = static_cast<uint8_t>((static_cast<uint16_t>(delta) * 255) / mx);

    int32_t hue = 0;
    if (c.r == mx) {
        hue = static_cast<int32_t>(c.g) - c.b;
        hue = (hue * 60) / delta;
    } else if (c.g == mx) {
        hue = static_cast<int32_t>(c.b) - c.r;
        hue = (hue * 60) / delta + 120;
    } else {
        hue = static_cast<int32_t>(c.r) - c.g;
        hue = (hue * 60) / delta + 240;
    }

    if (hue < 0) hue += 360;
    result.h = static_cast<uint16_t>(hue % 360);

    return result;
}

// ============================================================================
//  Static: colour utilities
// ============================================================================

Ws2812bStrip::Color Ws2812bStrip::wheel(uint8_t wheel_pos)
{
    // Full saturation, full value rainbow wheel
    return hsvToRgb(Hsv{static_cast<uint16_t>(static_cast<uint16_t>(wheel_pos) * 360 / 256), 255, 255});
}

Ws2812bStrip::Color Ws2812bStrip::lerp(const Color& a, const Color& b, uint8_t t)
{
    const uint16_t inv = 255 - t;
    return Color{
        static_cast<uint8_t>((static_cast<uint16_t>(a.r) * inv + static_cast<uint16_t>(b.r) * t) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(a.g) * inv + static_cast<uint16_t>(b.g) * t) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(a.b) * inv + static_cast<uint16_t>(b.b) * t) / 255),
    };
}
