#include "wireless/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void fail(const char* expression, int line)
{
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

void test_can_fd_roundtrip()
{
    can::Frame input{};
    input.id = 0x18FF50E5;
    input.extended = true;
    input.fd = true;
    input.bitrate_switch = true;
    input.size = 64;
    for (std::size_t index = 0; index < input.size; ++index) {
        input.data[index] = static_cast<uint8_t>(index ^ 0xA5U);
    }

    wireless::Packet packet{};
    CHECK(wireless::make_can_packet(
        input,
        {.origin = wireless::FrameOrigin::physical_bus,
         .bus = 0,
         .timestamp_us = 0x0102030405060708ULL},
        0x10203040,
        packet));
    wireless::WirePacket wire{};
    CHECK(wireless::encode_packet(packet, wire));

    wireless::Packet decoded{};
    const auto decode_result = wireless::decode_packet(
        std::span<const uint8_t>(wire.bytes.data(), wire.size), decoded);
    CHECK(decode_result.status == wireless::DecodeStatus::ok);
    CHECK(decode_result.consumed == wire.size);
    CHECK(decoded.sequence == 0x10203040);

    can::Frame output{};
    wireless::CanMetadata metadata{};
    CHECK(wireless::parse_can_packet(decoded, output, metadata));
    CHECK(output.id == input.id);
    CHECK(output.extended && output.fd && output.bitrate_switch && !output.remote);
    CHECK(output.size == input.size);
    CHECK(std::equal(input.data.begin(), input.data.end(), output.data.begin()));
    CHECK(metadata.origin == wireless::FrameOrigin::physical_bus);
    CHECK(metadata.timestamp_us == 0x0102030405060708ULL);
}

void test_crc_and_validation()
{
    wireless::Packet packet{};
    packet.type = wireless::MessageType::ping;
    packet.sequence = 7;
    packet.payload_size = 4;
    packet.payload[0] = 1;
    packet.payload[1] = 2;
    packet.payload[2] = 3;
    packet.payload[3] = 4;
    wireless::WirePacket wire{};
    CHECK(wireless::encode_packet(packet, wire));
    wire.bytes[wireless::kHeaderSize + 1] ^= 0x80;
    wireless::Packet decoded{};
    CHECK(wireless::decode_packet(
              std::span<const uint8_t>(wire.bytes.data(), wire.size), decoded)
              .status == wireless::DecodeStatus::invalid);

    can::Frame invalid{};
    invalid.fd = false;
    invalid.size = 9;
    CHECK(!wireless::make_can_packet(invalid, {}, 0, packet));
}

void test_fragmented_stream_and_resynchronization()
{
    wireless::Packet first{};
    first.type = wireless::MessageType::hello;
    first.sequence = 11;
    wireless::Packet second{};
    second.type = wireless::MessageType::status_request;
    second.sequence = 12;
    wireless::WirePacket first_wire{};
    wireless::WirePacket second_wire{};
    CHECK(wireless::encode_packet(first, first_wire));
    CHECK(wireless::encode_packet(second, second_wire));

    wireless::StreamDecoder decoder;
    const std::array<uint8_t, 3> garbage{0xAA, 0x55, 0x00};
    CHECK(decoder.append(garbage));
    CHECK(decoder.append(std::span<const uint8_t>(first_wire.bytes.data(), 5)));
    wireless::Packet output{};
    CHECK(decoder.next(output) == wireless::DecodeStatus::incomplete);
    CHECK(decoder.append(std::span<const uint8_t>(first_wire.bytes.data() + 5,
                                                  first_wire.size - 5)));
    CHECK(decoder.append(
        std::span<const uint8_t>(second_wire.bytes.data(), second_wire.size)));
    CHECK(decoder.next(output) == wireless::DecodeStatus::ok);
    CHECK(output.type == wireless::MessageType::hello && output.sequence == 11);
    CHECK(decoder.next(output) == wireless::DecodeStatus::ok);
    CHECK(output.type == wireless::MessageType::status_request && output.sequence == 12);
    CHECK(decoder.next(output) == wireless::DecodeStatus::incomplete);
}

} // namespace

int main()
{
    test_can_fd_roundtrip();
    test_crc_and_validation();
    test_fragmented_stream_and_resynchronization();
    std::puts("wireless protocol tests passed");
    return 0;
}
