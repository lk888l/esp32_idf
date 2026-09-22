#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace connectivity {
struct CommandResult {
    uint32_t ticket = 0, id = 0;
    bool applied = false;
    int32_t error = 0;
};
// Owner supplies synchronization. Tickets are independent of client IDs.
// Results expire after 16 submissions and all expire on reboot.
class CommandHistory {
public:
    static constexpr size_t capacity = 16;
    uint32_t next_ticket() { if (++sequence_ == 0) ++sequence_; return sequence_; }
    void queued(uint32_t ticket, uint32_t id) { entries_[ticket % capacity] = {ticket, id, false, 0}; }
    void finish(uint32_t ticket, int32_t error)
    {
        auto& entry = entries_[ticket % capacity];
        if (entry.ticket == ticket) { entry.applied = true; entry.error = error; }
    }
    CommandResult find(uint32_t ticket) const
    {
        const auto& entry = entries_[ticket % capacity];
        return entry.ticket == ticket ? entry : CommandResult{};
    }
private:
    uint32_t sequence_ = 0;
    std::array<CommandResult, capacity> entries_{};
};
} // namespace connectivity
