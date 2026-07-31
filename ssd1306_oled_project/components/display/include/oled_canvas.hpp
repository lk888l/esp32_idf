#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "u8g2.h"
}

namespace display {

enum class Font : std::uint8_t { small, medium, large, numeric };

class OledCanvas {
public:
    explicit OledCanvas(u8g2_t& u8g2) : u8g2_(u8g2) {}

    std::uint16_t width() const;
    std::uint16_t height() const;
    void clear(std::uint8_t intensity = 0);

    void drawPixel(int x, int y, std::uint8_t intensity = 255);
    void drawLine(int x0, int y0, int x1, int y1, std::uint8_t intensity = 255);
    void drawRect(int x, int y, int width, int height, std::uint8_t intensity = 255);
    void fillRect(int x, int y, int width, int height, std::uint8_t intensity = 255);
    void drawCircle(int cx, int cy, int radius, std::uint8_t intensity = 255);
    void fillCircle(int cx, int cy, int radius, std::uint8_t intensity = 255);
    void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                      std::uint8_t intensity = 255);
    void drawArc(int cx, int cy, int radius, int start_degrees, int end_degrees,
                 std::uint8_t intensity = 255);
    void drawProgressBar(int x, int y, int width, int height, float percent,
                         std::uint8_t intensity = 255);
    void drawBitmap(int x, int y, int width, int height, const std::uint8_t* xbm);
    void drawGrayBitmap(int x, int y, int width, int height,
                        const std::uint8_t* gray8, std::size_t stride = 0);

    void setFont(Font font);
    int textWidth(const char* utf8) const;
    void drawText(int x, int baseline_y, const char* utf8,
                  std::uint8_t intensity = 255);

private:
    bool ditherOn(int x, int y, std::uint8_t intensity) const;
    bool inBounds(int x, int y) const;
    void setPixel(int x, int y, bool on);

    u8g2_t& u8g2_;
    std::array<std::uint8_t, 1024> text_scratch_{};
};

} // namespace display
