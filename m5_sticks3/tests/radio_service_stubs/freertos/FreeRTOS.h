#pragma once
#include "../../stubs/freertos/FreeRTOS.h"
#include <mutex>
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
