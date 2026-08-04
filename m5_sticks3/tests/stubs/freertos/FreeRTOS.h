#pragma once

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;

constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdFAIL = 0;

#define pdMS_TO_TICKS(milliseconds) static_cast<TickType_t>(milliseconds)
