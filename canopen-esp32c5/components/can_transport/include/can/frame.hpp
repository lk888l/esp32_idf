#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace can {

inline constexpr std::size_t kClassicMaxPayload = 8;
inline constexpr std::size_t kFdMaxPayload = 64;

struct Frame {
    uint32_t id = 0;
    uint8_t size = 0;
    bool extended = false;
    bool remote = false;
    bool fd = false;
    bool bitrate_switch = false;
    std::array<uint8_t, kFdMaxPayload> data{};

    [[nodiscard]] bool valid() const
    {
        const uint32_t max_id = extended ? 0x1FFFFFFFU : 0x7FFU;
        const std::size_t max_payload = fd ? kFdMaxPayload : kClassicMaxPayload;
        return id <= max_id && size <= max_payload && !(remote && fd) &&
               (!bitrate_switch || fd);
    }

    [[nodiscard]] std::span<const uint8_t> payload() const
    {
        return {data.data(), size};
    }
};

enum class SendResult : uint8_t {
    ok,
    invalid_frame,
    not_ready,
    queue_full,
    timeout,
    io_error,
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual SendResult send(const Frame& frame, uint32_t timeout_ms) = 0;
};

} // namespace can

