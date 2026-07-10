#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace button {

enum class ButtonEventType : uint8_t {
    Click,
    LongPress,
};

struct ButtonEvent {
    gpio_num_t gpio;
    ButtonEventType type;
    uint32_t timestamp_ms;
};

struct ButtonConfig {
    gpio_num_t gpio;
    bool active_low = true;
    gpio_pull_mode_t pull_mode = GPIO_PULLUP_ONLY;
    uint16_t debounce_ms = 20;
    uint16_t long_press_ms = 800;
};

// A fixed-capacity GPIO button service.  One interrupt handler per GPIO wakes a
// shared worker task; no button is polled and no task is allocated per button.
class GpioButtonService final {
public:
    static constexpr std::size_t kMaxButtons = 16;

    explicit GpioButtonService(std::size_t event_queue_length = 16,
                               uint32_t task_stack_size = 2560,
                               UBaseType_t task_priority = tskIDLE_PRIORITY + 2);
    ~GpioButtonService();

    bool add_button(const ButtonConfig& config);
    bool start();
    bool stop(TickType_t timeout = pdMS_TO_TICKS(1000));

    bool try_pop(ButtonEvent& event);
    bool wait_pop(ButtonEvent& event, TickType_t timeout);
    uint32_t dropped_event_count() const;

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    std::size_t button_count() const { return button_count_; }

private:
    struct ButtonSlot {
        GpioButtonService* service = nullptr;
        ButtonConfig config{};
        volatile TickType_t last_edge_tick = 0;
        volatile uint32_t edge_generation = 0;
        uint32_t handled_generation = 0;
        TickType_t press_tick = 0;
        bool pressed = false;
        bool long_press_reported = false;
        uint32_t notification_bit = 0;
    };

    static void IRAM_ATTR gpio_isr(void* arg);
    static void task_entry(void* arg);

    void worker_loop();
    TickType_t process_buttons(TickType_t now);
    void publish(const ButtonSlot& slot, ButtonEventType type, TickType_t now);
    bool configure_button(ButtonSlot& slot);
    void remove_button_handlers();
    bool is_pressed(const ButtonSlot& slot) const;

    std::array<ButtonSlot, kMaxButtons> buttons_{};
    std::size_t button_count_ = 0;
    std::size_t event_queue_length_;
    uint32_t task_stack_size_;
    UBaseType_t task_priority_;
    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint32_t> dropped_events_{0};
    bool started_ = false;

    GpioButtonService(const GpioButtonService&) = delete;
    GpioButtonService& operator=(const GpioButtonService&) = delete;
};

} // namespace button
