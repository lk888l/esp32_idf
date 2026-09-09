#include "infrared_protocol.hpp"
#include "infrared_raw_mailbox.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace infrared::protocol;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void same_frame(const Frame& first, const Frame& second) {
    check(first.count == second.count && first.carrier_hz == second.carrier_hz && first.duty_percent == second.duty_percent, "stored metadata roundtrip");
    for (size_t i = 0; i < first.count; ++i) check(first.pulses[i].mark == second.pulses[i].mark && first.pulses[i].duration_us == second.pulses[i].duration_us, "stored pulse roundtrip");
}
void nec_tests() {
    Frame frame{};
    check(encode_nec(0x34, 0x56, false, frame), "encode standard NEC");
    check(frame.count == 67 && frame.pulses[0].duration_us == 9000 && frame.pulses[1].duration_us == 4500 && frame.pulses[66].mark, "canonical NEC envelope");
    auto decoded = decode_nec(frame);
    check(decoded.valid && !decoded.extended && decoded.address == 0x34 && decoded.command == 0x56 && decoded.raw == 0xA956CB34, "known NEC wire vector LSB-first");
    for (uint16_t address = 0; address < 256; ++address) {
        for (unsigned command = 0; command < 256; command += 17) {
            check(encode_nec(address, uint8_t(command), false, frame), "encode standard address domain");
            decoded = decode_nec(frame);
            check(decoded.valid && !decoded.extended && decoded.address == address && decoded.command == command, "roundtrip standard address domain");
        }
    }
    check(encode_nec(0x1234, 0xE7, true, frame), "encode extended NEC");
    decoded = decode_nec(frame);
    check(decoded.valid && decoded.extended && decoded.address == 0x1234 && decoded.command == 0xE7, "extended 16-bit address preserved");
    check(!encode_nec(0x1234, 0x55, false, frame), "reject implicit standard address truncation");
    encode_nec(0x1234, 0xE7, true, frame);
    frame.pulses[3 + 24 * 2].duration_us = frame.pulses[3 + 24 * 2].duration_us == 560 ? 1690 : 560;
    check(!decode_nec(frame).valid, "reject invalid command complement");
    encode_nec(0x34, 0x56, false, frame);
    frame.pulses[5].duration_us = 1000;
    check(!decode_nec(frame).valid, "reject ambiguous bit timing");
    encode_nec(0x34, 0x56, false, frame);
    frame.pulses[10].mark = false;
    check(!decode_nec(frame).valid, "reject malformed mark sequence");
    encode_nec(0x34, 0x56, false, frame);
    frame.count = 66;
    check(!decode_nec(frame).valid, "reject missing stop mark");
    encode_nec(0x34, 0x56, false, frame);
    frame.pulses[frame.count++] = {20000, false};
    check(decode_nec(frame).valid, "accept RMT trailing idle space");
    encode_nec(0x34, 0x56, false, frame);
    for (size_t i = 0; i < frame.count; ++i) frame.pulses[i].duration_us = uint16_t(frame.pulses[i].duration_us * 9U / 10U);
    check(decode_nec(frame).valid, "accept realistic timing tolerance");
    frame.pulses[0].duration_us = 6000;
    check(!decode_nec(frame).valid, "reject out-of-window header");
}
void repeat_tests() {
    Frame full{}, repeat{};
    encode_nec(0x22, 0x71, false, full);
    encode_nec_repeat(repeat);
    const auto raw_repeat = decode_nec(repeat);
    check(raw_repeat.repeat && !raw_repeat.valid, "repeat does not invent an address");
    NecDecoder decoder;
    check(!decoder.decode(repeat, 0).valid, "orphan repeat rejected");
    check(decoder.decode(full, 1000).valid, "complete frame establishes context");
    auto out = decoder.decode(repeat, 1110);
    check(out.valid && out.repeat && out.address == 0x22 && out.command == 0x71, "repeat inherits recent command");
    check(decoder.decode(repeat, 1220).valid, "held key refreshes context");
    check(!decoder.decode(repeat, 1381).valid, "stale repeat rejected");
    decoder.decode(full, 2000);
    check(!decoder.decode(repeat, 1999).valid, "time regression rejected");
    decoder.reset();
    check(!decoder.decode(repeat, 2100).valid, "explicit reset invalidates context");
    decoder.decode(full, 3000);
    full.pulses[0].duration_us = 20;
    check(!decoder.decode(full, 3010).valid, "unrelated frame rejected");
    check(!decoder.decode(repeat, 3100).valid, "unrelated frame clears repeat context");
}
void storage_tests() {
    Frame frame{}, restored{};
    encode_nec(0xC124, 0x52, true, frame);
    frame.carrier_hz = 40000; frame.duty_percent = 25;
    std::array<uint8_t, kMaxSerializedBytes> bytes{};
    const auto size = serialize(frame, bytes.data(), bytes.size());
    check(size == 16 + frame.count * 2 && deserialize(bytes.data(), size, restored), "serialize with CRC");
    same_frame(frame, restored);
    for (size_t cut = 0; cut < size; ++cut) check(!deserialize(bytes.data(), cut, restored), "all truncated encodings rejected");
    for (size_t position = 0; position < size; ++position) {
        bytes[position] ^= 0x01;
        check(!deserialize(bytes.data(), size, restored), "every single-byte corruption rejected");
        bytes[position] ^= 0x01;
    }
    check(!serialize(frame, bytes.data(), size - 1), "short output buffer rejected");
    check(!serialize(frame, nullptr, bytes.size()), "null output rejected");
    check(!deserialize(nullptr, size, restored), "null input rejected");
    check(!deserialize(bytes.data(), size + 1, restored), "trailing garbage rejected");
    frame.count = kMaxPulses;
    for (size_t i = 0; i < frame.count; ++i) frame.pulses[i] = {100, i % 2 == 0};
    const auto maximum_size = serialize(frame, bytes.data(), bytes.size());
    check(maximum_size == kMaxSerializedBytes && deserialize(bytes.data(), maximum_size, restored), "maximum raw envelope roundtrip");
    same_frame(frame, restored);
    frame.count = kMaxPulses + 1;
    check(!valid_frame(frame) && !serialize(frame, bytes.data(), bytes.size()), "overlong pulse count rejected");
    frame.count = std::numeric_limits<size_t>::max();
    check(!serialize(frame, bytes.data(), bytes.size()), "adversarial pulse count cannot overflow storage");
    frame.count = kMaxPulses;
    for (auto& pulse : frame.pulses) pulse.duration_us = 32767;
    check(!valid_frame(frame), "bounded total frame duration");
    encode_nec(1, 2, false, frame);
    frame.pulses[0].duration_us = 0;
    check(!valid_frame(frame), "zero-duration internal pulse rejected");
    frame.pulses[0].duration_us = 32768;
    check(!valid_frame(frame), "RMT 15-bit duration overflow rejected");
    encode_nec(1, 2, false, frame);
    frame.carrier_hz = 60001;
    check(!valid_frame(frame), "carrier out of range rejected");
    frame.carrier_hz = 38000; frame.duty_percent = 51;
    check(!valid_frame(frame), "carrier duty capped");
}
void mailbox_tests() {
    infrared::RawFrameMailbox mailbox;
    Frame input{}, output{};
    encode_nec(0x34, 0x56, false, input);
    check(mailbox.try_put(input), "raw mailbox accepts a copied frame");
    input.pulses[0].duration_us = 1;
    check(!mailbox.try_put(input), "raw mailbox rejects a second pending payload");
    check(mailbox.try_take(output) && output.pulses[0].duration_us == 9000, "caller mutation cannot alter the owned frame");
    check(!mailbox.pending() && !mailbox.try_take(output), "taking payload releases mailbox exactly once");
    encode_nec(1, 2, false, input);
    check(mailbox.try_put(input), "raw mailbox reusable after transmit");
    mailbox.clear();
    check(!mailbox.pending() && !mailbox.try_take(output), "stop discards pending payload");
    check(mailbox.try_put(input), "raw mailbox reusable after stop");
    mailbox.clear();
    input.count = kMaxPulses + 1;
    check(!mailbox.try_put(input) && !mailbox.pending(), "invalid payload never claims a mailbox slot");
}
void malformed_input_tests() {
    // Deterministic bounded fuzz protects the storage parser and decoder from
    // NVS corruption and arbitrary light/noise data, under ASan/UBSan as well.
    std::array<uint8_t, kMaxSerializedBytes> bytes{};
    Frame frame{};
    uint32_t seed = 0x1258F7A3;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        for (auto& byte : bytes) { seed ^= seed << 13U; seed ^= seed >> 17U; seed ^= seed << 5U; byte = uint8_t(seed); }
        const size_t size = seed % bytes.size();
        (void)deserialize(bytes.data(), size, frame);
        frame.count = seed % (kMaxPulses + 2);
        for (size_t i = 0; i < kMaxPulses; ++i) frame.pulses[i] = {uint16_t(bytes[2 * i] | uint16_t(bytes[2 * i + 1]) << 8U), bool(i & 1U)};
        (void)decode_nec(frame);
    }
}
}
int main() {
    nec_tests(); repeat_tests(); storage_tests(); mailbox_tests(); malformed_input_tests();
    std::puts("infrared protocol tests passed");
}
