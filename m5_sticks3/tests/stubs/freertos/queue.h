#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "FreeRTOS.h"

struct StaticQueue_t {
    size_t capacity = 0;
    size_t item_size = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    uint8_t* storage = nullptr;
};

using QueueHandle_t = StaticQueue_t*;

inline QueueHandle_t xQueueCreateStatic(UBaseType_t capacity,
                                        UBaseType_t item_size,
                                        uint8_t* storage,
                                        StaticQueue_t* queue)
{
    if (!capacity || !item_size || !storage || !queue) {
        return nullptr;
    }
    *queue = StaticQueue_t{
        .capacity = capacity,
        .item_size = item_size,
        .storage = storage,
    };
    return queue;
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t)
{
    if (!queue || queue->count == queue->capacity) {
        return pdFAIL;
    }
    std::memcpy(queue->storage + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1) % queue->capacity;
    ++queue->count;
    return pdPASS;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t)
{
    if (!queue || queue->count == 0) {
        return pdFAIL;
    }
    std::memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdPASS;
}

inline BaseType_t xQueueReset(QueueHandle_t queue)
{
    if (!queue) {
        return pdFAIL;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    return pdPASS;
}

inline TickType_t xTaskGetTickCount()
{
    return 0;
}
