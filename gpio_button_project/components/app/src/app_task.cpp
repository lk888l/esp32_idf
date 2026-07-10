#include "app_task.hpp"

AppTask::AppTask(std::string_view name,
                 uint32_t stack_size,
                 UBaseType_t priority,
                 BaseType_t core)
    : name_(name)
    , stack_size_(stack_size)
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
    if (handle_.load(std::memory_order_acquire) != nullptr ||
        running_.load(std::memory_order_acquire)) {
        return false;
    }

    exit_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    TaskHandle_t handle = nullptr;
    const BaseType_t ret = xTaskCreatePinnedToCore(
        taskEntry,
        name_.c_str(),
        stack_size_,
        this,
        priority_,
        &handle,
        core_);

    if (ret != pdPASS) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    if (running_.load(std::memory_order_acquire)) {
        handle_.store(handle, std::memory_order_release);
    }

    return true;
}

bool AppTask::stop(TickType_t timeout)
{
    TaskHandle_t handle = handle_.load(std::memory_order_acquire);
    if (handle == nullptr && !running_.load(std::memory_order_acquire)) {
        return true;
    }

    exit_requested_.store(true, std::memory_order_release);

    if (handle != nullptr && xTaskGetCurrentTaskHandle() == handle) {
        return false;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    while (running_.load(std::memory_order_acquire) ||
           handle_.load(std::memory_order_acquire) != nullptr) {
        if (timeout != portMAX_DELAY && (xTaskGetTickCount() - start_tick) >= timeout) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

void AppTask::taskEntry(void* arg)
{
    auto* self = static_cast<AppTask*>(arg);

    self->main();
    self->cleanup();
    self->handle_.store(nullptr, std::memory_order_release);
    self->running_.store(false, std::memory_order_release);

    vTaskDelete(nullptr);
}
