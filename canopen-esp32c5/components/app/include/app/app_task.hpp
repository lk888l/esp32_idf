#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace app {

class Task {
public:
    Task(std::string_view name, uint32_t stack_size, UBaseType_t priority);
    virtual ~Task();
    bool start();
    bool stop(TickType_t timeout = pdMS_TO_TICKS(1000));
    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }

protected:
    virtual void run() = 0;
    [[nodiscard]] bool stop_requested() const
    {
        return stop_requested_.load(std::memory_order_acquire);
    }

private:
    static void entry(void* context);
    std::string_view name_;
    uint32_t stack_size_;
    UBaseType_t priority_;
    std::atomic<TaskHandle_t> handle_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace app

