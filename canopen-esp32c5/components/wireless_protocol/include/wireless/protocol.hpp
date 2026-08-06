#pragma once

#include "can/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wireless {

inline constexpr std::array<uint8_t, 2> kMagic{'H', 'X'};
inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kCrcSize = 4;
inline constexpr std::size_t kMaxPayloadSize = 128;
inline constexpr std::size_t kMaxPacketSize = kHeaderSize + kMaxPayloadSize + kCrcSize;

enum class MessageType : uint8_t {
    hello = 0x01,
    device_info = 0x02,
    auth_request = 0x03,
    auth_challenge = 0x04,
    auth_response = 0x05,
    auth_result = 0x06,
    can_frame = 0x10,
    status_request = 0x20,
    status_response = 0x21,
    wifi_credentials = 0x30,
    wifi_result = 0x31,
    ping = 0x40,
    pong = 0x41,
    error = 0x7F,
};

enum PacketFlags : uint16_t {
    packet_flag_none = 0,
    packet_flag_response = 1U << 0U,
    packet_flag_error = 1U << 1U,
};

enum class FrameOrigin : uint8_t { physical_bus = 0, local_node = 1, wireless_client = 2 };

struct Packet {
    MessageType type = MessageType::error;
    uint16_t flags = packet_flag_none;
    uint32_t sequence = 0;
    uint16_t payload_size = 0;
    std::array<uint8_t, kMaxPayloadSize> payload{};

    [[nodiscard]] bool valid() const { return payload_size <= payload.size(); }
};

struct WirePacket {
    uint16_t size = 0;
    std::array<uint8_t, kMaxPacketSize> bytes{};
};

enum class DecodeStatus : uint8_t { ok, incomplete, invalid };

struct DecodeResult {
    DecodeStatus status = DecodeStatus::incomplete;
    std::size_t consumed = 0;
};

struct CanMetadata {
    FrameOrigin origin = FrameOrigin::physical_bus;
    uint8_t bus = 0;
    uint64_t timestamp_us = 0;
};

[[nodiscard]] uint32_t crc32_ieee(std::span<const uint8_t> bytes);
[[nodiscard]] bool encode_packet(const Packet& packet, WirePacket& wire);
[[nodiscard]] DecodeResult decode_packet(std::span<const uint8_t> bytes, Packet& packet);
[[nodiscard]] bool make_can_packet(const can::Frame& frame,
                                   const CanMetadata& metadata,
                                   uint32_t sequence,
                                   Packet& packet);
[[nodiscard]] bool parse_can_packet(const Packet& packet,
                                    can::Frame& frame,
                                    CanMetadata& metadata);

class StreamDecoder {
public:
    static constexpr std::size_t kCapacity = kMaxPacketSize * 3;

    bool append(std::span<const uint8_t> bytes);
    DecodeStatus next(Packet& packet);
    void reset() { size_ = 0; }
    [[nodiscard]] std::size_t buffered() const { return size_; }

private:
    void discard(std::size_t count);
    std::array<uint8_t, kCapacity> buffer_{};
    std::size_t size_ = 0;
};

} // namespace wireless
