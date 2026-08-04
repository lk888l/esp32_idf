#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"

namespace model {

struct MotionSample {
    float accel_g[3]{};
    float gyro_dps[3]{};
    float quaternion[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float roll_deg = 0.0f;
    float pitch_deg = 0.0f;
    float yaw_deg = 0.0f;
    uint32_t sample_count = 0;
    bool rest_detected = false;
    bool valid = false;
};

class MotionState {
public:
    static MotionState& instance();
    void publish(const MotionSample& sample);
    MotionSample snapshot();

private:
    MotionState() = default;

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    MotionSample sample_{};
};

} // namespace model

