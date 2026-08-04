#include "motion_state.hpp"

namespace model {

MotionState& MotionState::instance()
{
    static MotionState state;
    return state;
}

void MotionState::publish(const MotionSample& sample)
{
    portENTER_CRITICAL(&lock_);
    sample_ = sample;
    portEXIT_CRITICAL(&lock_);
}

MotionSample MotionState::snapshot()
{
    portENTER_CRITICAL(&lock_);
    const MotionSample copy = sample_;
    portEXIT_CRITICAL(&lock_);
    return copy;
}

} // namespace model
