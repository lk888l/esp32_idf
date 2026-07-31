#include "oled_display.hpp"

#include <cstring>
#include <span>

namespace display {
namespace {

const u8g2_cb_t* rotationCallback(Rotation rotation)
{
    switch (rotation) {
    case Rotation::deg0:
        return U8G2_R0;
    case Rotation::deg90:
        return U8G2_R1;
    case Rotation::deg180:
        return U8G2_R2;
    case Rotation::deg270:
        return U8G2_R3;
    default:
        return U8G2_R0;
    }
}

} // namespace

const char* toString(DisplayStatus status)
{
    switch (status) {
    case DisplayStatus::ok:
        return "ok";
    case DisplayStatus::invalid_argument:
        return "invalid_argument";
    case DisplayStatus::not_initialized:
        return "not_initialized";
    case DisplayStatus::buffer_overflow:
        return "buffer_overflow";
    case DisplayStatus::transport_error:
        return "transport_error";
    default:
        return "unknown";
    }
}

OledDisplay::OledDisplay(bsp::I2CDevice& device, OledDisplayConfig config)
    : device_(device)
    , config_(config)
{
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2_, rotationCallback(config_.rotation), byteCallback, gpioDelayCallback);
    u8g2_SetUserPtr(&u8g2_, this);
}

void OledDisplay::resetStatus()
{
    last_status_ = DisplayStatus::ok;
    transfer_size_ = 0;
}

DisplayStatus OledDisplay::initialize()
{
    initialized_ = false;
    if (config_.delay_ms == nullptr || config_.delay_us == nullptr) {
        last_status_ = DisplayStatus::invalid_argument;
        return last_status_;
    }

    resetStatus();
    u8g2_InitDisplay(&u8g2_);
    if (last_status_ != DisplayStatus::ok) {
        return last_status_;
    }

    u8g2_SetPowerSave(&u8g2_, 0);
    u8g2_SetContrast(&u8g2_, config_.contrast);
    u8g2_ClearBuffer(&u8g2_);
    u8g2_SendBuffer(&u8g2_);
    initialized_ = last_status_ == DisplayStatus::ok;
    return last_status_;
}

DisplayStatus OledDisplay::present()
{
    if (!initialized_) {
        return DisplayStatus::not_initialized;
    }

    resetStatus();
    u8g2_SendBuffer(&u8g2_);
    return last_status_;
}

DisplayStatus OledDisplay::setPowerSave(bool enabled)
{
    if (!initialized_) {
        return DisplayStatus::not_initialized;
    }

    resetStatus();
    u8g2_SetPowerSave(&u8g2_, enabled ? 1 : 0);
    return last_status_;
}

DisplayStatus OledDisplay::setContrast(std::uint8_t contrast)
{
    if (!initialized_) {
        return DisplayStatus::not_initialized;
    }

    resetStatus();
    u8g2_SetContrast(&u8g2_, contrast);
    return last_status_;
}

std::uint16_t OledDisplay::width() const
{
    return u8g2_GetDisplayWidth(const_cast<u8g2_t*>(&u8g2_));
}

std::uint16_t OledDisplay::height() const
{
    return u8g2_GetDisplayHeight(const_cast<u8g2_t*>(&u8g2_));
}

const std::uint8_t* OledDisplay::framebufferData() const
{
    return u8g2_GetBufferPtr(const_cast<u8g2_t*>(&u8g2_));
}

std::size_t OledDisplay::framebufferSize() const
{
    return static_cast<std::size_t>(
               u8g2_GetBufferTileHeight(const_cast<u8g2_t*>(&u8g2_))) *
           static_cast<std::size_t>(
               u8g2_GetBufferTileWidth(const_cast<u8g2_t*>(&u8g2_))) *
           8U;
}

std::uint8_t OledDisplay::byteCallback(u8x8_t* u8x8,
                                       std::uint8_t message,
                                       std::uint8_t value,
                                       void* data)
{
    auto* self = static_cast<OledDisplay*>(u8x8_GetUserPtr(u8x8));
    return self == nullptr ? 0 : self->onByteMessage(message, value, data);
}

std::uint8_t OledDisplay::gpioDelayCallback(u8x8_t* u8x8,
                                            std::uint8_t message,
                                            std::uint8_t value,
                                            void*)
{
    auto* self = static_cast<OledDisplay*>(u8x8_GetUserPtr(u8x8));
    return self == nullptr ? 0 : self->onDelayMessage(message, value);
}

std::uint8_t OledDisplay::onByteMessage(std::uint8_t message,
                                        std::uint8_t count,
                                        const void* data)
{
    switch (message) {
    case U8X8_MSG_BYTE_INIT:
        return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:
        transfer_size_ = 0;
        return 1;
    case U8X8_MSG_BYTE_SEND:
        if (data == nullptr || transfer_size_ + count > transfer_buffer_.size()) {
            last_status_ = data == nullptr ? DisplayStatus::invalid_argument
                                           : DisplayStatus::buffer_overflow;
            return 0;
        }
        std::memcpy(transfer_buffer_.data() + transfer_size_, data, count);
        transfer_size_ += count;
        return 1;
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (transfer_size_ == 0) {
            return 1;
        }
        if (device_.write(std::span<const std::uint8_t>(
                transfer_buffer_.data(), transfer_size_)) != bsp::I2CStatus::ok) {
            last_status_ = DisplayStatus::transport_error;
            transfer_size_ = 0;
            return 0;
        }
        transfer_size_ = 0;
        return 1;
    case U8X8_MSG_BYTE_SET_DC:
        return 1;
    default:
        return 0;
    }
}

std::uint8_t OledDisplay::onDelayMessage(std::uint8_t message,
                                         std::uint8_t value)
{
    switch (message) {
    case U8X8_MSG_DELAY_MILLI:
        config_.delay_ms(value);
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        config_.delay_us(static_cast<std::uint32_t>(value) * 10U);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
        if (value > 0) {
            config_.delay_us((static_cast<std::uint32_t>(value) + 9U) / 10U);
        }
        return 1;
    case U8X8_MSG_DELAY_NANO:
        if (value > 0) {
            config_.delay_us((static_cast<std::uint32_t>(value) + 999U) / 1000U);
        }
        return 1;
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
    case U8X8_MSG_DELAY_I2C:
    case U8X8_MSG_GPIO_RESET:
    case U8X8_MSG_GPIO_I2C_CLOCK:
    case U8X8_MSG_GPIO_I2C_DATA:
        return 1;
    default:
        return 1;
    }
}

} // namespace display
