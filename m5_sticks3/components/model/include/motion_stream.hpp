#pragma once
#include "freertos/FreeRTOS.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace model
{
struct StreamSample
{
    uint64_t time_us = 0; // Monotonic acquisition estimate; never wall clock.
    uint32_t sequence = 0;
    float roll = 0, pitch = 0, yaw = 0;
    bool valid = false;
};
// Single producer; bounded, nonblocking copies for independent subscribers.
class MotionStream
{
  public:
    static MotionStream &instance();
    void request(bool enabled)
    {
        requested_.store(enabled);
    }
    bool requested() const
    {
        return requested_.load();
    }
    void publish(StreamSample sample);
    uint32_t latest();
    size_t read(uint32_t &after, StreamSample *out, size_t capacity, uint32_t &lost);

  private:
    std::atomic<bool> requested_{false};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    std::array<StreamSample, 128> samples_{};
    uint32_t sequence_ = 0;
};
} // namespace model
