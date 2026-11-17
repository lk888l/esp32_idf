#include "motion_stream.hpp"
#include <algorithm>
namespace model
{
MotionStream &MotionStream::instance()
{
    static MotionStream value;
    return value;
}
void MotionStream::publish(StreamSample sample)
{
    portENTER_CRITICAL(&lock_);
    sample.sequence = ++sequence_;
    samples_[sequence_ % samples_.size()] = sample;
    portEXIT_CRITICAL(&lock_);
}
uint32_t MotionStream::latest()
{
    portENTER_CRITICAL(&lock_);
    auto n = sequence_;
    portEXIT_CRITICAL(&lock_);
    return n;
}
size_t MotionStream::read(uint32_t &after, StreamSample *out, size_t capacity, uint32_t &lost)
{
    portENTER_CRITICAL(&lock_);
    uint32_t available = sequence_ - after;
    if (available > samples_.size())
    {
        lost += available - samples_.size();
        after = sequence_ - samples_.size();
        available = samples_.size();
    }
    const size_t n = std::min(size_t(available), capacity);
    for (size_t i = 0; i < n; ++i)
        out[i] = samples_[++after % samples_.size()];
    portEXIT_CRITICAL(&lock_);
    return n;
}
} // namespace model
