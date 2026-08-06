#include "wireless/protocol.hpp"

#include <algorithm>
#include <cstring>

namespace wireless {
namespace {

constexpr uint8_t kCanFlagExtended = 1U << 0U;
constexpr uint8_t kCanFlagRemote = 1U << 1U;
constexpr uint8_t kCanFlagFd = 1U << 2U;
constexpr uint8_t kCanFlagBitrateSwitch = 1U << 3U;
constexpr std::size_t kCanMetadataSize = 16;

void put_u16(uint8_t* output, uint16_t value)
{
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
}

void put_u32(uint8_t* output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void put_u64(uint8_t* output, uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

uint16_t get_u16(const uint8_t* input)
{
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t get_u32(const uint8_t* input)
{
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

uint64_t get_u64(const uint8_t* input)
{
    uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

bool known_type(MessageType type)
{
    switch (type) {
    case MessageType::hello:
    case MessageType::device_info:
    case MessageType::auth_request:
    case MessageType::auth_challenge:
    case MessageType::auth_response:
    case MessageType::auth_result:
    case MessageType::can_frame:
    case MessageType::status_request:
    case MessageType::status_response:
    case MessageType::wifi_credentials:
    case MessageType::wifi_result:
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::error:
        return true;
    }
    return false;
}

} // namespace

uint32_t crc32_ieee(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool encode_packet(const Packet& packet, WirePacket& wire)
{
    if (!packet.valid() || !known_type(packet.type)) {
        return false;
    }
    wire = {};
    wire.size = static_cast<uint16_t>(kHeaderSize + packet.payload_size + kCrcSize);
    wire.bytes[0] = kMagic[0];
    wire.bytes[1] = kMagic[1];
    wire.bytes[2] = kProtocolVersion;
    wire.bytes[3] = static_cast<uint8_t>(packet.type);
    put_u16(wire.bytes.data() + 4, packet.flags);
    put_u16(wire.bytes.data() + 6, packet.payload_size);
    put_u32(wire.bytes.data() + 8, packet.sequence);
    std::copy_n(packet.payload.begin(), packet.payload_size, wire.bytes.begin() + kHeaderSize);
    const uint32_t crc = crc32_ieee(
        std::span<const uint8_t>(wire.bytes.data(), wire.size - kCrcSize));
    put_u32(wire.bytes.data() + wire.size - kCrcSize, crc);
    return true;
}

DecodeResult decode_packet(std::span<const uint8_t> bytes, Packet& packet)
{
    if (bytes.size() < kHeaderSize) {
        return {DecodeStatus::incomplete, 0};
    }
    if (bytes[0] != kMagic[0] || bytes[1] != kMagic[1] ||
        bytes[2] != kProtocolVersion) {
        return {DecodeStatus::invalid, 1};
    }
    const auto type = static_cast<MessageType>(bytes[3]);
    const uint16_t payload_size = get_u16(bytes.data() + 6);
    if (!known_type(type) || payload_size > kMaxPayloadSize) {
        return {DecodeStatus::invalid, 1};
    }
    const std::size_t total = kHeaderSize + payload_size + kCrcSize;
    if (bytes.size() < total) {
        return {DecodeStatus::incomplete, 0};
    }
    const uint32_t expected_crc = get_u32(bytes.data() + total - kCrcSize);
    if (crc32_ieee(bytes.first(total - kCrcSize)) != expected_crc) {
        return {DecodeStatus::invalid, 1};
    }
    packet = {};
    packet.type = type;
    packet.flags = get_u16(bytes.data() + 4);
    packet.payload_size = payload_size;
    packet.sequence = get_u32(bytes.data() + 8);
    std::copy_n(bytes.begin() + kHeaderSize, payload_size, packet.payload.begin());
    return {DecodeStatus::ok, total};
}

bool make_can_packet(const can::Frame& frame,
                     const CanMetadata& metadata,
                     uint32_t sequence,
                     Packet& packet)
{
    if (!frame.valid()) {
        return false;
    }
    packet = {};
    packet.type = MessageType::can_frame;
    packet.sequence = sequence;
    packet.payload_size = static_cast<uint16_t>(kCanMetadataSize + frame.size);
    put_u32(packet.payload.data(), frame.id);
    packet.payload[4] = frame.size;
    packet.payload[5] = static_cast<uint8_t>((frame.extended ? kCanFlagExtended : 0U) |
                                             (frame.remote ? kCanFlagRemote : 0U) |
                                             (frame.fd ? kCanFlagFd : 0U) |
                                             (frame.bitrate_switch ? kCanFlagBitrateSwitch : 0U));
    packet.payload[6] = static_cast<uint8_t>(metadata.origin);
    packet.payload[7] = metadata.bus;
    put_u64(packet.payload.data() + 8, metadata.timestamp_us);
    std::copy_n(frame.data.begin(), frame.size, packet.payload.begin() + kCanMetadataSize);
    return true;
}

bool parse_can_packet(const Packet& packet, can::Frame& frame, CanMetadata& metadata)
{
    if (packet.type != MessageType::can_frame || packet.payload_size < kCanMetadataSize) {
        return false;
    }
    frame = {};
    frame.id = get_u32(packet.payload.data());
    frame.size = packet.payload[4];
    const uint8_t flags = packet.payload[5];
    if ((flags & 0xF0U) != 0U ||
        packet.payload_size != static_cast<std::size_t>(kCanMetadataSize + frame.size) ||
        packet.payload[6] > static_cast<uint8_t>(FrameOrigin::wireless_client)) {
        return false;
    }
    frame.extended = (flags & kCanFlagExtended) != 0;
    frame.remote = (flags & kCanFlagRemote) != 0;
    frame.fd = (flags & kCanFlagFd) != 0;
    frame.bitrate_switch = (flags & kCanFlagBitrateSwitch) != 0;
    if (!frame.valid()) {
        return false;
    }
    metadata.origin = static_cast<FrameOrigin>(packet.payload[6]);
    metadata.bus = packet.payload[7];
    metadata.timestamp_us = get_u64(packet.payload.data() + 8);
    std::copy_n(packet.payload.begin() + kCanMetadataSize, frame.size, frame.data.begin());
    return true;
}

bool StreamDecoder::append(std::span<const uint8_t> bytes)
{
    if (bytes.size() > buffer_.size() - size_) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), buffer_.begin() + size_);
    size_ += bytes.size();
    return true;
}

DecodeStatus StreamDecoder::next(Packet& packet)
{
    while (size_ >= 2 && (buffer_[0] != kMagic[0] || buffer_[1] != kMagic[1])) {
        discard(1);
    }
    if (size_ < 2) {
        return DecodeStatus::incomplete;
    }
    const DecodeResult result =
        decode_packet(std::span<const uint8_t>(buffer_.data(), size_), packet);
    if (result.status == DecodeStatus::ok) {
        discard(result.consumed);
    } else if (result.status == DecodeStatus::invalid) {
        discard(result.consumed == 0 ? 1 : result.consumed);
    }
    return result.status;
}

void StreamDecoder::discard(std::size_t count)
{
    count = std::min(count, size_);
    if (count < size_) {
        std::memmove(buffer_.data(), buffer_.data() + count, size_ - count);
    }
    size_ -= count;
}

} // namespace wireless
