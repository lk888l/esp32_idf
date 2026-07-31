#include "oled_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace display {
namespace {

constexpr std::uint8_t kBayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

constexpr double kPi = 3.14159265358979323846;

} // namespace

std::uint16_t OledCanvas::width() const
{
    return u8g2_GetDisplayWidth(const_cast<u8g2_t*>(&u8g2_));
}

std::uint16_t OledCanvas::height() const
{
    return u8g2_GetDisplayHeight(const_cast<u8g2_t*>(&u8g2_));
}

bool OledCanvas::inBounds(int x, int y) const
{
    return x >= 0 && y >= 0 && x < static_cast<int>(width()) && y < static_cast<int>(height());
}

bool OledCanvas::ditherOn(int x, int y, std::uint8_t intensity) const
{
    const unsigned level = (static_cast<unsigned>(intensity) * 16U + 254U) / 255U;
    return kBayer4x4[y & 3][x & 3] < level;
}

void OledCanvas::setPixel(int x, int y, bool on)
{
    if (!inBounds(x, y)) { return; }
    u8g2_SetDrawColor(&u8g2_, on ? 1 : 0);
    u8g2_DrawPixel(&u8g2_, static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(y));
}

void OledCanvas::clear(std::uint8_t intensity)
{
    if (intensity == 0) {
        u8g2_ClearBuffer(&u8g2_);
        return;
    }
    for (int y = 0; y < static_cast<int>(height()); ++y) {
        for (int x = 0; x < static_cast<int>(width()); ++x) {
            setPixel(x, y, ditherOn(x, y, intensity));
        }
    }
}

void OledCanvas::drawPixel(int x, int y, std::uint8_t intensity)
{
    if (!inBounds(x, y)) { return; }
    setPixel(x, y, ditherOn(x, y, intensity));
}

void OledCanvas::drawLine(int x0, int y0, int x1, int y1, std::uint8_t intensity)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        drawPixel(x0, y0, intensity);
        if (x0 == x1 && y0 == y1) { break; }
        const int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

void OledCanvas::drawRect(int x, int y, int rect_width, int rect_height,
                          std::uint8_t intensity)
{
    if (rect_width <= 0 || rect_height <= 0) { return; }
    drawLine(x, y, x + rect_width - 1, y, intensity);
    drawLine(x, y + rect_height - 1, x + rect_width - 1, y + rect_height - 1, intensity);
    drawLine(x, y, x, y + rect_height - 1, intensity);
    drawLine(x + rect_width - 1, y, x + rect_width - 1, y + rect_height - 1, intensity);
}

void OledCanvas::fillRect(int x, int y, int rect_width, int rect_height,
                          std::uint8_t intensity)
{
    if (rect_width <= 0 || rect_height <= 0) { return; }
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(static_cast<int>(width()), x + rect_width);
    const int bottom = std::min(static_cast<int>(height()), y + rect_height);
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            drawPixel(px, py, intensity);
        }
    }
}

void OledCanvas::drawCircle(int cx, int cy, int radius, std::uint8_t intensity)
{
    if (radius < 0) { return; }
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        drawPixel(cx + x, cy + y, intensity); drawPixel(cx + y, cy + x, intensity);
        drawPixel(cx - y, cy + x, intensity); drawPixel(cx - x, cy + y, intensity);
        drawPixel(cx - x, cy - y, intensity); drawPixel(cx - y, cy - x, intensity);
        drawPixel(cx + y, cy - x, intensity); drawPixel(cx + x, cy - y, intensity);
        ++y;
        if (error < 0) { error += 2 * y + 1; }
        else { --x; error += 2 * (y - x + 1); }
    }
}

void OledCanvas::fillCircle(int cx, int cy, int radius, std::uint8_t intensity)
{
    if (radius < 0) { return; }
    for (int y = -radius; y <= radius; ++y) {
        const int half_width = static_cast<int>(std::sqrt(radius * radius - y * y));
        drawLine(cx - half_width, cy + y, cx + half_width, cy + y, intensity);
    }
}

void OledCanvas::drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                              std::uint8_t intensity)
{
    drawLine(x0, y0, x1, y1, intensity);
    drawLine(x1, y1, x2, y2, intensity);
    drawLine(x2, y2, x0, y0, intensity);
}

void OledCanvas::drawArc(int cx, int cy, int radius, int start_degrees, int end_degrees,
                         std::uint8_t intensity)
{
    if (radius < 0) { return; }
    while (end_degrees < start_degrees) { end_degrees += 360; }
    end_degrees = std::min(end_degrees, start_degrees + 720);
    int last_x = cx + static_cast<int>(std::lround(radius * std::cos(start_degrees * kPi / 180.0)));
    int last_y = cy + static_cast<int>(std::lround(radius * std::sin(start_degrees * kPi / 180.0)));
    for (int angle = start_degrees + 1; angle <= end_degrees; ++angle) {
        const int px = cx + static_cast<int>(std::lround(radius * std::cos(angle * kPi / 180.0)));
        const int py = cy + static_cast<int>(std::lround(radius * std::sin(angle * kPi / 180.0)));
        drawLine(last_x, last_y, px, py, intensity);
        last_x = px;
        last_y = py;
    }
}

void OledCanvas::drawProgressBar(int x, int y, int bar_width, int bar_height, float percent,
                                 std::uint8_t intensity)
{
    if (bar_width < 3 || bar_height < 3) { return; }
    drawRect(x, y, bar_width, bar_height, 255);
    const float clamped = std::max(0.0F, std::min(100.0F, percent));
    const int fill_width = static_cast<int>((bar_width - 2) * clamped / 100.0F + 0.5F);
    fillRect(x + 1, y + 1, fill_width, bar_height - 2, intensity);
    fillRect(x + 1 + fill_width, y + 1, bar_width - 2 - fill_width, bar_height - 2, 0);
}

void OledCanvas::drawBitmap(int x, int y, int bitmap_width, int bitmap_height,
                            const std::uint8_t* xbm)
{
    if (xbm == nullptr || bitmap_width <= 0 || bitmap_height <= 0) { return; }
    u8g2_SetDrawColor(&u8g2_, 1);
    u8g2_DrawXBM(&u8g2_, x, y, static_cast<u8g2_uint_t>(bitmap_width),
                 static_cast<u8g2_uint_t>(bitmap_height), xbm);
}

void OledCanvas::drawGrayBitmap(int x, int y, int bitmap_width, int bitmap_height,
                                const std::uint8_t* gray8, std::size_t stride)
{
    if (gray8 == nullptr || bitmap_width <= 0 || bitmap_height <= 0) { return; }
    if (stride == 0) { stride = static_cast<std::size_t>(bitmap_width); }
    if (stride < static_cast<std::size_t>(bitmap_width)) { return; }
    for (int py = 0; py < bitmap_height; ++py) {
        for (int px = 0; px < bitmap_width; ++px) {
            drawPixel(x + px, y + py, gray8[static_cast<std::size_t>(py) * stride + px]);
        }
    }
}

void OledCanvas::setFont(Font font)
{
    switch (font) {
    case Font::small: u8g2_SetFont(&u8g2_, u8g2_font_5x7_tr); break;
    case Font::medium: u8g2_SetFont(&u8g2_, u8g2_font_6x10_tr); break;
    case Font::large: u8g2_SetFont(&u8g2_, u8g2_font_helvB12_tr); break;
    case Font::numeric: u8g2_SetFont(&u8g2_, u8g2_font_logisoso16_tn); break;
    }
    u8g2_SetFontMode(&u8g2_, 1);
}

int OledCanvas::textWidth(const char* utf8) const
{
    if (utf8 == nullptr) { return 0; }
    return static_cast<int>(u8g2_GetUTF8Width(const_cast<u8g2_t*>(&u8g2_), utf8));
}

void OledCanvas::drawText(int x, int baseline_y, const char* utf8,
                          std::uint8_t intensity)
{
    if (utf8 == nullptr || *utf8 == '\0') { return; }
    auto* buffer = u8g2_GetBufferPtr(&u8g2_);
    const std::size_t buffer_size = static_cast<std::size_t>(u8g2_GetBufferTileHeight(&u8g2_)) *
                                    u8g2_GetBufferTileWidth(&u8g2_) * 8U;
    if (buffer == nullptr || buffer_size > text_scratch_.size()) { return; }
    std::memcpy(text_scratch_.data(), buffer, buffer_size);

    u8g2_SetDrawColor(&u8g2_, 1);
    u8g2_DrawUTF8(&u8g2_, x, baseline_y, utf8);
    if (intensity == 255) { return; }

    const int raw_width = static_cast<int>(u8g2_GetBufferTileWidth(&u8g2_)) * 8;
    for (std::size_t index = 0; index < buffer_size; ++index) {
        const std::uint8_t added = static_cast<std::uint8_t>(buffer[index] & ~text_scratch_[index]);
        if (added == 0) { continue; }
        const int px = static_cast<int>(index % static_cast<std::size_t>(raw_width));
        const int page_y = static_cast<int>(index / static_cast<std::size_t>(raw_width)) * 8;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
            if ((added & mask) != 0 && !ditherOn(px, page_y + bit, intensity)) {
                buffer[index] = static_cast<std::uint8_t>(buffer[index] & ~mask);
            }
        }
    }
}

} // namespace display
