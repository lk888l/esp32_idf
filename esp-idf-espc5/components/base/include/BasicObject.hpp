#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class BasicObject {
public:
    struct SignalContext {
        TaskHandle_t task = nullptr;
        uint32_t bits = 0;
    };

    static bool emit(const SignalContext& signal)
    {
        if (signal.task == nullptr || signal.bits == 0) {
            return false;
        }

        return xTaskNotify(signal.task, signal.bits, eSetBits) == pdPASS;
    }

    static bool emitFromISR(const SignalContext& signal, BaseType_t* higher_priority_task_woken)
    {
        if (signal.task == nullptr || signal.bits == 0) {
            return false;
        }

        return xTaskNotifyFromISR(signal.task,
                                  signal.bits,
                                  eSetBits,
                                  higher_priority_task_woken) == pdPASS;
    }
};
