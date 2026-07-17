#include "app_task.hpp"

namespace app {

AppTask::AppTask(const char* name,
                 uint32_t stack_size_bytes,
                 UBaseType_t priority,
                 BaseType_t core) noexcept
    : name_(name)
    , stack_size_bytes_(stack_size_bytes)
    , priority_(priority)
    , core_(core)
{
}

AppTask::~AppTask()
{
    stop();
}

bool AppTask::start()
{
    if (handle_.load(std::memory_order_acquire) != nullptr || isRunning()) {
        return false;
    }

    exit_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    TaskHandle_t task_handle = nullptr;
    const BaseType_t result = xTaskCreatePinnedToCore(taskEntry,
                                                      name_,
                                                      stack_size_bytes_,
                                                      this,
                                                      priority_,
                                                      &task_handle,
                                                      core_);
    if (result != pdPASS) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    if (running_.load(std::memory_order_acquire)) {
        handle_.store(task_handle, std::memory_order_release);
    }
    return true;
}

bool AppTask::stop(TickType_t timeout)
{
    TaskHandle_t task_handle = handle_.load(std::memory_order_acquire);
    if (task_handle == nullptr && !isRunning()) {
        return true;
    }

    exit_requested_.store(true, std::memory_order_release);
    if (task_handle == xTaskGetCurrentTaskHandle()) {
        return false;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    while (isRunning() || handle_.load(std::memory_order_acquire) != nullptr) {
        if (timeout != portMAX_DELAY && (xTaskGetTickCount() - start_tick) >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

void AppTask::taskEntry(void* context)
{
    auto* task = static_cast<AppTask*>(context);
    task->run();
    task->cleanup();
    task->handle_.store(nullptr, std::memory_order_release);
    task->running_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace app
