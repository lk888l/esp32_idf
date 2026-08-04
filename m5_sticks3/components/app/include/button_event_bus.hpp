#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace app {

enum class ButtonId : uint8_t {
    key1,
    key2,
};

enum class ButtonAction : uint8_t {
    pressed,
    released,
};

struct ButtonEvent {
    ButtonId id;
    ButtonAction action;
    TickType_t timestamp;
};

class ButtonEventBus {
public:
    static constexpr size_t kQueueCapacity = 8;
    static constexpr size_t kSubscriberCapacity = 4;

    using Callback = void (*)(void* context, const ButtonEvent& event);

    class Subscription {
    public:
        bool valid() const { return slot_ < kSubscriberCapacity; }

    private:
        friend class ButtonEventBus;

        static constexpr uint8_t kInvalidSlot = UINT8_MAX;
        uint8_t slot_ = kInvalidSlot;
        uint16_t generation_ = 0;
    };

    ButtonEventBus();

    ButtonEventBus(const ButtonEventBus&) = delete;
    ButtonEventBus& operator=(const ButtonEventBus&) = delete;

    // Subscription changes and dispatching belong to the application task. publish() is
    // the only operation called by the button producer task.
    Subscription subscribe(Callback callback, void* context);
    bool unsubscribe(Subscription subscription);

    bool publish(const ButtonEvent& event);
    size_t dispatch_pending(size_t max_events = kQueueCapacity);
    void clear();

    uint32_t dropped_events() const
    {
        return dropped_events_.load(std::memory_order_relaxed);
    }

private:
    struct SubscriberSlot {
        Callback callback = nullptr;
        void* context = nullptr;
        uint16_t generation = 0;
    };

    void report_dropped_events();

    alignas(ButtonEvent)
        std::array<uint8_t, kQueueCapacity * sizeof(ButtonEvent)> queue_storage_{};
    StaticQueue_t queue_state_{};
    QueueHandle_t queue_ = nullptr;
    std::array<SubscriberSlot, kSubscriberCapacity> subscribers_{};
    std::atomic<uint32_t> dropped_events_{0};
    uint32_t reported_dropped_events_ = 0;
    TickType_t last_drop_report_ = 0;
};

} // namespace app
