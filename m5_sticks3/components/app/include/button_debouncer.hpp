#pragma once

#include <optional>

#include "button_event_bus.hpp"

namespace app {

class ButtonDebouncer {
public:
    ButtonDebouncer(ButtonId id, TickType_t debounce_ticks)
        : id_(id), debounce_ticks_(debounce_ticks)
    {
    }

    std::optional<ButtonEvent> update(bool pressed, TickType_t now)
    {
        if (pressed != raw_pressed_) {
            raw_pressed_ = pressed;
            changed_at_ = now;
        }

        if (stable_pressed_ == raw_pressed_ || now - changed_at_ < debounce_ticks_) {
            return std::nullopt;
        }

        stable_pressed_ = raw_pressed_;
        return ButtonEvent{
            .id = id_,
            .action = stable_pressed_ ? ButtonAction::pressed : ButtonAction::released,
            .timestamp = now,
        };
    }

private:
    ButtonId id_;
    TickType_t debounce_ticks_;
    bool raw_pressed_ = false;
    bool stable_pressed_ = false;
    TickType_t changed_at_ = 0;
};

} // namespace app
