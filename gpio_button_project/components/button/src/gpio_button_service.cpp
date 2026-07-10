#include "gpio_button_service.hpp"

#include <algorithm>
#include <limits>

#include "esp_err.h"

namespace button {
namespace {

TickType_t ms_to_ticks_at_least_one(uint32_t milliseconds)
{
    return std::max<TickType_t>(1, pdMS_TO_TICKS(milliseconds));
}

uint32_t ticks_to_ms(TickType_t ticks)
{
    return static_cast<uint32_t>(ticks) * portTICK_PERIOD_MS;
}

} // namespace

GpioButtonService::GpioButtonService(std::size_t event_queue_length,
                                     uint32_t task_stack_size,
                                     UBaseType_t task_priority)
    : event_queue_length_(event_queue_length)
    , task_stack_size_(task_stack_size)
    , task_priority_(task_priority)
{
}

GpioButtonService::~GpioButtonService()
{
    stop();
}

bool GpioButtonService::add_button(const ButtonConfig& config)
{
    if (started_ || button_count_ >= kMaxButtons || config.gpio < GPIO_NUM_0 ||
        config.gpio >= GPIO_NUM_MAX || config.debounce_ms == 0 ||
        config.long_press_ms <= config.debounce_ms) {
        return false;
    }

    for (std::size_t i = 0; i < button_count_; ++i) {
        if (buttons_[i].config.gpio == config.gpio) {
            return false;
        }
    }

    auto& slot = buttons_[button_count_];
    slot.service = this;
    slot.config = config;
    slot.notification_bit = 1U << button_count_;
    ++button_count_;
    return true;
}

bool GpioButtonService::start()
{
    if (started_ || button_count_ == 0 || event_queue_length_ == 0) {
        return false;
    }

    event_queue_ = xQueueCreate(event_queue_length_, sizeof(ButtonEvent));
    if (event_queue_ == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < button_count_; ++i) {
        if (!configure_button(buttons_[i])) {
            remove_button_handlers();
            vQueueDelete(event_queue_);
            event_queue_ = nullptr;
            return false;
        }
    }

    // The ISR service is process-wide. ESP_ERR_INVALID_STATE means another
    // component already installed it, which is safe to share.
    const esp_err_t isr_service_result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_service_result != ESP_OK && isr_service_result != ESP_ERR_INVALID_STATE) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    if (xTaskCreate(task_entry,
                    "gpio_buttons",
                    task_stack_size_,
                    this,
                    task_priority_,
                    &task_handle_) != pdPASS) {
        running_.store(false, std::memory_order_release);
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return false;
    }

    for (std::size_t i = 0; i < button_count_; ++i) {
        if (gpio_isr_handler_add(buttons_[i].config.gpio, gpio_isr, &buttons_[i]) != ESP_OK) {
            remove_button_handlers();
            stop(pdMS_TO_TICKS(1000));
            return false;
        }
    }

    started_ = true;
    return true;
}

bool GpioButtonService::stop(TickType_t timeout)
{
    if (!started_ && !running_.load(std::memory_order_acquire)) {
        return true;
    }

    remove_button_handlers();
    stop_requested_.store(true, std::memory_order_release);
    if (task_handle_ != nullptr) {
        xTaskNotify(task_handle_, 0, eNoAction);
    }

    const TickType_t start_tick = xTaskGetTickCount();
    while (running_.load(std::memory_order_acquire)) {
        if (timeout != portMAX_DELAY && (xTaskGetTickCount() - start_tick) >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }

    task_handle_ = nullptr;
    if (event_queue_ != nullptr) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
    started_ = false;
    return true;
}

bool GpioButtonService::try_pop(ButtonEvent& event)
{
    return event_queue_ != nullptr && xQueueReceive(event_queue_, &event, 0) == pdTRUE;
}

bool GpioButtonService::wait_pop(ButtonEvent& event, TickType_t timeout)
{
    return event_queue_ != nullptr && xQueueReceive(event_queue_, &event, timeout) == pdTRUE;
}

uint32_t GpioButtonService::dropped_event_count() const
{
    return dropped_events_.load(std::memory_order_relaxed);
}

void IRAM_ATTR GpioButtonService::gpio_isr(void* arg)
{
    auto* slot = static_cast<ButtonSlot*>(arg);
    slot->last_edge_tick = xTaskGetTickCountFromISR();
    slot->edge_generation = slot->edge_generation + 1U;

    // The handler does not read GPIO, allocate memory, or send to the event
    // queue; it only wakes the shared worker task.
    auto* service = slot->service;

    BaseType_t higher_priority_task_woken = pdFALSE;
    const TaskHandle_t task = service->task_handle_;
    if (task != nullptr) {
        xTaskNotifyFromISR(task,
                           slot->notification_bit,
                           eSetBits,
                           &higher_priority_task_woken);
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void GpioButtonService::task_entry(void* arg)
{
    static_cast<GpioButtonService*>(arg)->worker_loop();
    vTaskDelete(nullptr);
}

void GpioButtonService::worker_loop()
{
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const TickType_t wait = process_buttons(xTaskGetTickCount());
        uint32_t ignored_notification_bits = 0;
        xTaskNotifyWait(0, std::numeric_limits<uint32_t>::max(),
                        &ignored_notification_bits, wait);
    }

    running_.store(false, std::memory_order_release);
}

TickType_t GpioButtonService::process_buttons(TickType_t now)
{
    TickType_t next_wait = portMAX_DELAY;

    for (std::size_t i = 0; i < button_count_; ++i) {
        auto& slot = buttons_[i];
        const uint32_t edge_generation = slot.edge_generation;
        if (edge_generation != slot.handled_generation) {
            const TickType_t debounce_ticks = ms_to_ticks_at_least_one(slot.config.debounce_ms);
            const TickType_t elapsed = now - slot.last_edge_tick;
            if (elapsed >= debounce_ticks) {
                slot.handled_generation = edge_generation;
                const bool pressed = is_pressed(slot);
                if (pressed != slot.pressed) {
                    if (pressed) {
                        slot.press_tick = now;
                        slot.long_press_reported = false;
                    } else if (!slot.long_press_reported) {
                        publish(slot, ButtonEventType::Click, now);
                    }
                    slot.pressed = pressed;
                }
            } else {
                next_wait = std::min(next_wait, debounce_ticks - elapsed);
            }
        }

        if (slot.pressed && !slot.long_press_reported) {
            const TickType_t long_press_ticks = ms_to_ticks_at_least_one(slot.config.long_press_ms);
            const TickType_t elapsed = now - slot.press_tick;
            if (elapsed >= long_press_ticks) {
                // A release which occurred just before the long-press deadline
                // may still be inside the debounce window. Do not turn that
                // short press into a long press before the release settles.
                if (edge_generation == slot.handled_generation || is_pressed(slot)) {
                    slot.long_press_reported = true;
                    publish(slot, ButtonEventType::LongPress, now);
                }
            } else {
                next_wait = std::min(next_wait, long_press_ticks - elapsed);
            }
        }
    }

    return next_wait;
}

void GpioButtonService::publish(const ButtonSlot& slot, ButtonEventType type, TickType_t now)
{
    const ButtonEvent event{slot.config.gpio, type, ticks_to_ms(now)};
    if (xQueueSend(event_queue_, &event, 0) != pdTRUE) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool GpioButtonService::configure_button(ButtonSlot& slot)
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << slot.config.gpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = (slot.config.pull_mode == GPIO_PULLUP_ONLY ||
                         slot.config.pull_mode == GPIO_PULLUP_PULLDOWN)
                            ? GPIO_PULLUP_ENABLE
                            : GPIO_PULLUP_DISABLE;
    config.pull_down_en = (slot.config.pull_mode == GPIO_PULLDOWN_ONLY ||
                           slot.config.pull_mode == GPIO_PULLUP_PULLDOWN)
                              ? GPIO_PULLDOWN_ENABLE
                              : GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_ANYEDGE;
    if (gpio_config(&config) != ESP_OK) {
        return false;
    }

    slot.pressed = is_pressed(slot);
    slot.press_tick = xTaskGetTickCount();
    slot.long_press_reported = false;
    slot.last_edge_tick = slot.press_tick;
    slot.handled_generation = slot.edge_generation;
    return true;
}

void GpioButtonService::remove_button_handlers()
{
    for (std::size_t i = 0; i < button_count_; ++i) {
        gpio_isr_handler_remove(buttons_[i].config.gpio);
    }
}

bool GpioButtonService::is_pressed(const ButtonSlot& slot) const
{
    const int level = gpio_get_level(slot.config.gpio);
    return slot.config.active_low ? level == 0 : level != 0;
}

} // namespace button
