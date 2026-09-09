#pragma once

#include "infrared_protocol.hpp"

namespace infrared {
// One owned pending payload keeps command-queue memory bounded. Service owns the
// synchronization; this portable helper deliberately performs no allocation.
class RawFrameMailbox {
public:
    bool try_put(const protocol::Frame& frame) {
        if (pending_ || !protocol::valid_frame(frame)) return false;
        frame_ = frame;
        pending_ = true;
        return true;
    }
    bool try_take(protocol::Frame& output) {
        if (!pending_) return false;
        output = frame_;
        clear();
        return true;
    }
    void clear() { pending_ = false; frame_.count = 0; }
    bool pending() const { return pending_; }
private:
    protocol::Frame frame_{};
    bool pending_ = false;
};
} // namespace infrared
