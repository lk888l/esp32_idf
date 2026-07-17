#pragma once

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "noncopyable.hpp"

namespace app {

class AppTask : private base::NonCopyable {
public:
    AppTask(const char* name,
            uint32_t stack_size_bytes,
            UBaseType_t priority,
            BaseType_t core = tskNO_AFFINITY) noexcept;
    virtual ~AppTask();

    bool start();
    bool stop(TickType_t timeout = pdMS_TO_TICKS(1000));

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

protected:
    virtual void run() = 0;
    virtual void cleanup() {}

    bool shouldExit() const noexcept
    {
        return exit_requested_.load(std::memory_order_acquire);
    }

private:
    static void taskEntry(void* context);

    const char* name_;
    uint32_t stack_size_bytes_;
    UBaseType_t priority_;
    BaseType_t core_;
    std::atomic<TaskHandle_t> handle_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> exit_requested_{false};
};

} // namespace app
