#pragma once
#include <mutex>
struct StaticSemaphore_t { std::mutex mutex; };
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* value) { return value; }
inline int xSemaphoreTake(SemaphoreHandle_t value, TickType_t) { value->mutex.lock(); return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t value) { value->mutex.unlock(); return 1; }
