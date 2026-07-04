#include "app_task.hpp"

AppTask::AppTask(const std::string& name,
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
    if (running_)
    {
        return false;
    }

    exit_requested_ = false;

    BaseType_t ret =
        xTaskCreatePinnedToCore(
            taskEntry,
            name_.c_str(),
            stack_size_,
            this,
            priority_,
            &handle_,
            core_);

    return ret == pdPASS;
}

void AppTask::stop()
{
    if (!running_)
    {
        return;
    }

    // exit_requested_ = true;

    // while (running_)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(100));
    // }

    cleanup();

    running_ = false;

    if (handle_ != nullptr)
    {
        vTaskDelete(handle_);
        handle_ = nullptr;
    }
}

void AppTask::taskEntry(void* arg)
{
    auto* self = static_cast<AppTask*>(arg);

    self->running_ = true;

    self->main();

    self->cleanup();

    self->running_ = false;

    self->handle_ = nullptr;

    vTaskDelete(nullptr);
}