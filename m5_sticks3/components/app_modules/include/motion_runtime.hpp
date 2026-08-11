#pragma once

#include <cstdint>

namespace app_modules {

enum class MotionRuntimeState : uint8_t {
    stopped,
    starting,
    running,
    stopping,
    failed,
};

void request_motion_enabled(bool enabled);
bool motion_enabled_requested();
MotionRuntimeState motion_runtime_state();

} // namespace app_modules
