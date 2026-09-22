#pragma once
namespace model {
class MotionState {
public:
    struct Value { bool valid = false; unsigned sample_count = 0; double roll_deg = 0, pitch_deg = 0, yaw_deg = 0; };
    static MotionState& instance() { static MotionState value; return value; }
    Value snapshot() const { return {}; }
};
}
