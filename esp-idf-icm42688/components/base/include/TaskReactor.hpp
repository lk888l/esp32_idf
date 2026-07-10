#pragma once

#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>
#include <system_error>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BasicObject.hpp"

class TaskReactor : public BasicObject {
public:
    using SlotCallback = std::function<void()>;

    struct strCMD_t {
        std::string_view command;
        std::string_view args;
    };

    explicit TaskReactor(TaskHandle_t task_handle = nullptr)
        : reactor_task_(task_handle)
    {
    }

    void bindToCurrentTask()
    {
        reactor_task_ = xTaskGetCurrentTaskHandle();
    }

    TaskHandle_t taskHandle() const
    {
        return reactor_task_;
    }

    void stop()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (reactor_task_ != nullptr) {
            xTaskNotify(reactor_task_, stop_bit_, eSetBits);
        }
    }

    bool stopped() const
    {
        return stop_requested_.load(std::memory_order_acquire);
    }

    bool setSlot(uint32_t bit_mask, SlotCallback callback)
    {
        if (bit_mask == 0 || (bit_mask & (bit_mask - 1U)) != 0) {
            return false;
        }

        const int index = __builtin_ctz(bit_mask);
        if (index < 0 || index >= static_cast<int>(slots_.size())) {
            return false;
        }

        slots_[index] = std::move(callback);
        return true;
    }

    template<typename Sender, typename SignalMethod, typename SlotLambda>
    bool connect(Sender* sender, SignalMethod signal, SlotLambda slot)
    {
        if (sender == nullptr) {
            return false;
        }

        const uint32_t bit_mask = allocateNotifyBit();
        if (bit_mask == 0) {
            return false;
        }

        sender->bindReactor(signal, reactor_task_, bit_mask);
        return setSlot(bit_mask, [sender, signal, slot]() {
            (sender->*signal)(slot);
        });
    }

    void taskLoop(TickType_t ticks_to_wait = portMAX_DELAY,
                  const SlotCallback& after_notify = nullptr,
                  const SlotCallback& on_timeout = nullptr)
    {
        bindToCurrentTask();
        stop_requested_.store(false, std::memory_order_release);

        uint32_t notified_value = 0;
        TickType_t last_wake_time = xTaskGetTickCount();

        while (!stop_requested_.load(std::memory_order_acquire)) {
            TickType_t ticks_remaining = portMAX_DELAY;

            if (on_timeout && ticks_to_wait != portMAX_DELAY) {
                const TickType_t elapsed = xTaskGetTickCount() - last_wake_time;
                if (elapsed >= ticks_to_wait) {
                    on_timeout();
                    last_wake_time = xTaskGetTickCount();
                } else {
                    ticks_remaining = ticks_to_wait - elapsed;
                }
            }

            if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notified_value, ticks_remaining) == pdTRUE) {
                if ((notified_value & stop_bit_) != 0) {
                    stop_requested_.store(true, std::memory_order_release);
                }

                dispatch(notified_value & ~stop_bit_);

                if (after_notify) {
                    after_notify();
                }
            }
        }
    }

    static bool parseStrCMD(std::string_view input, strCMD_t& str_cmd)
    {
        const std::size_t start = input.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            str_cmd.command = {};
            str_cmd.args = {};
            return false;
        }

        input.remove_prefix(start);

        const std::size_t space_pos = input.find(' ');
        if (space_pos == std::string_view::npos) {
            str_cmd.command = input;
            str_cmd.args = {};
            return true;
        }

        str_cmd.command = input.substr(0, space_pos);
        std::string_view args = input.substr(space_pos + 1);
        const std::size_t first_arg = args.find_first_not_of(' ');
        if (first_arg == std::string_view::npos) {
            str_cmd.args = {};
        } else {
            args.remove_prefix(first_arg);
            str_cmd.args = args;
        }

        return true;
    }

    template<typename T>
    static bool parseStrArg(std::string_view& str_arg, T& value)
    {
        const std::size_t first = str_arg.find_first_not_of(' ');
        if (first == std::string_view::npos) {
            return false;
        }

        str_arg.remove_prefix(first);

        const std::size_t last = str_arg.find(' ');
        const std::string_view token = str_arg.substr(0, last);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc()) {
            return false;
        }

        if (last == std::string_view::npos) {
            str_arg = {};
        } else {
            str_arg.remove_prefix(last + 1);
        }

        return true;
    }

protected:
    uint32_t allocateNotifyBit() const
    {
        for (std::size_t i = 0; i < slots_.size() - 1; ++i) {
            if (!slots_[i]) {
                return 1UL << i;
            }
        }

        return 0;
    }

private:
    void dispatch(uint32_t notified_value)
    {
        for (std::size_t i = 0; notified_value != 0 && i < slots_.size(); ++i) {
            const uint32_t bit = 1UL << i;
            if ((notified_value & bit) != 0) {
                if (slots_[i]) {
                    slots_[i]();
                }
                notified_value &= ~bit;
            }
        }
    }

    static constexpr uint32_t stop_bit_ = 1UL << 31;

    TaskHandle_t reactor_task_ = nullptr;
    std::array<SlotCallback, 32> slots_{};
    std::atomic<bool> stop_requested_{false};
};
