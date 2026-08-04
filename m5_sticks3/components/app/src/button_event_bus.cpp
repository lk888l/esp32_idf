#include "button_event_bus.hpp"

#include "esp_log.h"

namespace app {
namespace {

constexpr char kTag[] = "button_events";
constexpr TickType_t kDropReportInterval = pdMS_TO_TICKS(1000);

} // namespace

ButtonEventBus::ButtonEventBus()
{
    queue_ = xQueueCreateStatic(kQueueCapacity,
                                sizeof(ButtonEvent),
                                queue_storage_.data(),
                                &queue_state_);
}

ButtonEventBus::Subscription ButtonEventBus::subscribe(Callback callback, void* context)
{
    if (!callback) {
        return {};
    }

    for (size_t index = 0; index < subscribers_.size(); ++index) {
        auto& slot = subscribers_[index];
        if (slot.callback) {
            continue;
        }

        ++slot.generation;
        if (slot.generation == 0) {
            ++slot.generation;
        }
        slot.context = context;
        slot.callback = callback;

        Subscription subscription;
        subscription.slot_ = static_cast<uint8_t>(index);
        subscription.generation_ = slot.generation;
        return subscription;
    }

    return {};
}

bool ButtonEventBus::unsubscribe(Subscription subscription)
{
    if (!subscription.valid()) {
        return false;
    }

    auto& slot = subscribers_[subscription.slot_];
    if (!slot.callback || slot.generation != subscription.generation_) {
        return false;
    }

    slot.callback = nullptr;
    slot.context = nullptr;
    return true;
}

bool ButtonEventBus::publish(const ButtonEvent& event)
{
    if (queue_ && xQueueSend(queue_, &event, 0) == pdPASS) {
        return true;
    }

    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t ButtonEventBus::dispatch_pending(size_t max_events)
{
    if (!queue_) {
        return 0;
    }

    size_t dispatched = 0;
    ButtonEvent event{};
    while (dispatched < max_events && xQueueReceive(queue_, &event, 0) == pdPASS) {
        for (const auto& subscriber : subscribers_) {
            if (subscriber.callback) {
                subscriber.callback(subscriber.context, event);
            }
        }
        ++dispatched;
    }

    report_dropped_events();
    return dispatched;
}

void ButtonEventBus::clear()
{
    if (queue_) {
        xQueueReset(queue_);
    }
}

void ButtonEventBus::report_dropped_events()
{
    const uint32_t dropped = dropped_events_.load(std::memory_order_relaxed);
    if (dropped == reported_dropped_events_) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (reported_dropped_events_ != 0 && now - last_drop_report_ < kDropReportInterval) {
        return;
    }

    ESP_LOGW(kTag, "button event queue full; dropped=%lu", dropped);
    reported_dropped_events_ = dropped;
    last_drop_report_ = now;
}

} // namespace app
