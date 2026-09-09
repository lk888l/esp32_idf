#include "infrared_protocol.hpp"

namespace infrared::protocol {
namespace {
bool near(uint16_t measured, uint16_t expected) {
    return measured >= expected * 3U / 4U && measured <= expected * 5U / 4U;
}
uint32_t crc_update(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    return crc;
}
uint32_t storage_crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) if (i < 12 || i >= 16) crc = crc_update(crc, data[i]);
    return ~crc;
}
uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8U | uint32_t(p[2]) << 16U | uint32_t(p[3]) << 24U;
}
void write32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8U * i));
}
}

uint32_t duration_us(const Frame& frame) {
    if (frame.count > kMaxPulses) return 0;
    uint32_t total = 0;
    for (size_t i = 0; i < frame.count; ++i) total += frame.pulses[i].duration_us;
    return total;
}
bool valid_frame(const Frame& frame) {
    if (!frame.count || frame.count > kMaxPulses || frame.carrier_hz < 20000 ||
        frame.carrier_hz > 60000 || !frame.duty_percent || frame.duty_percent > 50) return false;
    if (!frame.pulses[0].mark) return false;
    for (size_t i = 0; i < frame.count; ++i) {
        const auto& pulse = frame.pulses[i];
        if (!pulse.duration_us || pulse.duration_us > 32767 || (i && pulse.mark == frame.pulses[i - 1].mark)) return false;
    }
    return duration_us(frame) <= kMaxFrameDurationUs;
}
bool encode_nec(uint16_t address, uint8_t command, bool extended, Frame& out) {
    if (!extended && address > 0xFF) return false;
    out = {};
    const uint16_t wire_address = extended ? address : uint16_t(address | uint16_t(uint8_t(~address)) << 8U);
    const uint32_t raw = wire_address | uint32_t(command) << 16U | uint32_t(uint8_t(~command)) << 24U;
    out.pulses[out.count++] = {9000, true};
    out.pulses[out.count++] = {4500, false};
    for (unsigned i = 0; i < 32; ++i) {
        out.pulses[out.count++] = {560, true};
        out.pulses[out.count++] = {uint16_t(raw & (1UL << i) ? 1690 : 560), false};
    }
    out.pulses[out.count++] = {560, true};
    return true;
}
void encode_nec_repeat(Frame& out) {
    out = {};
    out.pulses[0] = {9000, true}; out.pulses[1] = {2250, false}; out.pulses[2] = {560, true};
    out.count = 3;
}
Decoded decode_nec(const Frame& frame) {
    Decoded out{};
    if (!valid_frame(frame)) return out;
    size_t count = frame.count;
    // The RMT idle threshold can appear as a trailing space, but never as data.
    if (count && !frame.pulses[count - 1].mark) --count;
    if (count < 3 || !near(frame.pulses[0].duration_us, 9000)) return out;
    if (count == 3 && near(frame.pulses[1].duration_us, 2250) && near(frame.pulses[2].duration_us, 560)) {
        out.repeat = true;
        return out;
    }
    if (count != 67 || !near(frame.pulses[1].duration_us, 4500) || !near(frame.pulses[66].duration_us, 560)) return out;
    uint32_t raw = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (!near(frame.pulses[2 + bit * 2].duration_us, 560)) return out;
        const auto space = frame.pulses[3 + bit * 2].duration_us;
        if (near(space, 1690)) raw |= uint32_t(1) << bit;
        else if (!near(space, 560)) return out;
    }
    const uint8_t command = raw >> 16U;
    if (uint8_t(raw >> 24U) != uint8_t(~command)) return out;
    const uint8_t address = raw;
    out.valid = true;
    out.extended = uint8_t(raw >> 8U) != uint8_t(~address);
    out.address = out.extended ? uint16_t(raw) : address;
    out.command = command;
    out.raw = raw;
    return out;
}
Decoded NecDecoder::decode(const Frame& frame, uint64_t timestamp_ms) {
    Decoded out = decode_nec(frame);
    if (out.repeat) {
        if (previous_.valid && timestamp_ms >= previous_ms_ && timestamp_ms - previous_ms_ <= 160) {
            out = previous_; out.repeat = true; previous_ms_ = timestamp_ms;
        }
    } else if (out.valid) { previous_ = out; previous_ms_ = timestamp_ms; }
    else reset();
    return out;
}
void NecDecoder::reset() { previous_ = {}; previous_ms_ = 0; }

size_t serialize(const Frame& frame, uint8_t* output, size_t capacity) {
    const size_t size = 16 + frame.count * 2;
    if (!output || !valid_frame(frame) || capacity < size) return 0;
    output[0] = 'I'; output[1] = 'R'; output[2] = 'F'; output[3] = '1';
    output[4] = 1; output[5] = frame.duty_percent;
    output[6] = uint8_t(frame.count); output[7] = uint8_t(frame.count >> 8U);
    write32(output + 8, frame.carrier_hz);
    for (size_t i = 0; i < frame.count; ++i) {
        const uint16_t v = frame.pulses[i].duration_us | (frame.pulses[i].mark ? 0x8000U : 0);
        output[16 + i * 2] = uint8_t(v); output[17 + i * 2] = uint8_t(v >> 8U);
    }
    write32(output + 12, storage_crc(output, size));
    return size;
}
bool deserialize(const uint8_t* data, size_t length, Frame& output) {
    if (!data || length < 16 || data[0] != 'I' || data[1] != 'R' || data[2] != 'F' || data[3] != '1' || data[4] != 1) return false;
    const size_t count = size_t(data[6]) | size_t(data[7]) << 8U;
    if (count == 0 || count > kMaxPulses || length != 16 + count * 2 || read32(data + 12) != storage_crc(data, length)) return false;
    output.count = 0;
    output.carrier_hz = read32(data + 8); output.duty_percent = data[5];
    for (size_t i = 0; i < count; ++i) {
        const uint16_t v = uint16_t(data[16 + 2 * i]) | uint16_t(data[17 + 2 * i]) << 8U;
        output.pulses[i] = {uint16_t(v & 0x7FFFU), bool(v & 0x8000U)};
    }
    output.count = count;
    if (!valid_frame(output)) { output.count = 0; return false; }
    return true;
}
} // namespace infrared::protocol
