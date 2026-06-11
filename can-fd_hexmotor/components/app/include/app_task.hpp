#pragma once

#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AppTask
{
public:
    AppTask(const std::string& name,
            uint32_t stack_size,
            UBaseType_t priority,
            BaseType_t core = tskNO_AFFINITY);

    virtual ~AppTask();

    bool start();
    void stop();

    bool isRunning() const{return running_;}

    const std::string& name() const{return name_;}

protected:
    virtual void main() = 0;
    virtual void cleanup() {}

    bool shouldExit() const
    {
        return exit_requested_;
    }

private:
    static void taskEntry(void* arg);

private:
    std::string name_;

    uint32_t stack_size_;
    UBaseType_t priority_;
    BaseType_t core_;

    TaskHandle_t handle_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> exit_requested_{false};
};