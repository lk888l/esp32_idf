// UDP v1 compatibility: espc3-integrated ad1ebef and wl1-studio b969bf7.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pc_monitor
{
inline constexpr uint16_t udp_port = 4443;
inline constexpr std::size_t max_packet_bytes = 1024;  // valid packets are smaller
inline constexpr uint32_t stale_after_ms = 3000;

struct Stats
{
    char session[33]{}, host[65]{}, gpu_name[97]{};
    uint32_t seq = 0;
    float cpu_pct = 0, gpu_pct = 0, gpu_temp_c = 0;
    uint64_t ram_used = 0, ram_total = 0, vram_used = 0, vram_total = 0;
    bool cpu_valid = false, gpu_valid = false, vram_valid = false, gpu_temp_valid = false;
};

struct Snapshot
{
    Stats stats{};
    bool listening = false, received = false, stale = true;
    uint32_t age_ms = 0, accepted = 0, rejected = 0, out_of_order = 0;
    int error = 0;  // latest socket errno; zero when transport is healthy
};

// Strict, bounded JSON v1 parser. Failure leaves output unchanged.
bool parse_sample(std::string_view packet, Stats& output);
// Modulo-u32 ordering; a fresh session accepts any initial sequence.
bool newer_sequence(uint32_t incoming, uint32_t previous);

// Transport-independent latest-value state, owned by the application task.
class LatestSample
{
   public:
    bool accept(const Stats& incoming, uint32_t received_ms);
    void reset();
    bool received() const { return received_; }
    bool stale(uint32_t now) const;
    uint32_t age_ms(uint32_t now) const;
    const Stats& stats() const { return stats_; }

   private:
    Stats stats_{};
    uint32_t received_ms_ = 0;
    bool received_ = false;
};
}  // namespace pc_monitor
