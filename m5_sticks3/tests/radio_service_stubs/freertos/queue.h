#pragma once
#include "FreeRTOS.h"
#include "../../stubs/freertos/queue.h"
inline unsigned uxQueueSpacesAvailable(QueueHandle_t queue) { return queue->capacity - queue->count; }
