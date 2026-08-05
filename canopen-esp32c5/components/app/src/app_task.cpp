#include "app/app_task.hpp"

namespace app {

Task::Task(std::string_view name, uint32_t stack_size, UBaseType_t priority)
    : name_(name), stack_size_(stack_size), priority_(priority)
{
}

Task::~Task()
{
    (void)stop();
}

bool Task::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    stop_requested_.store(false, std::memory_order_release);
    TaskHandle_t created = nullptr;
    const BaseType_t result =
        xTaskCreate(entry, name_.data(), stack_size_, this, priority_, &created);
    if (result != pdPASS) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    handle_.store(created, std::memory_order_release);
    return true;
}

bool Task::stop(TickType_t timeout)
{
    if (!running()) {
        return true;
    }
    stop_requested_.store(true, std::memory_order_release);
    const TickType_t begin = xTaskGetTickCount();
    while (running()) {
        if (timeout != portMAX_DELAY && xTaskGetTickCount() - begin >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void Task::entry(void* context)
{
    auto* self = static_cast<Task*>(context);
    self->run();
    self->handle_.store(nullptr, std::memory_order_release);
    self->running_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace app

