#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace infrared::protocol {

inline constexpr size_t kMaxPulses = 1024;
inline constexpr size_t kMaxSerializedBytes = 16 + kMaxPulses * 2;
inline constexpr uint32_t kDefaultCarrierHz = 38000;
inline constexpr uint32_t kMaxFrameDurationUs = 2000000;

// Demodulated envelope. mark=true emits the configured carrier, false is silence.
struct Pulse { uint16_t duration_us = 0; bool mark = false; };
struct Frame {
    std::array<Pulse, kMaxPulses> pulses{};
    size_t count = 0;
    uint32_t carrier_hz = kDefaultCarrierHz;
    uint8_t duty_percent = 33;
};
struct Decoded {
    bool valid = false;
    bool repeat = false;
    bool extended = false;
    uint16_t address = 0;
    uint8_t command = 0;
    uint32_t raw = 0;
};

bool valid_frame(const Frame& frame);
uint32_t duration_us(const Frame& frame);
bool encode_nec(uint16_t address, uint8_t command, bool extended, Frame& out);
void encode_nec_repeat(Frame& out);
Decoded decode_nec(const Frame& frame);
// Repeat frames inherit an address only from a recent complete NEC frame.
class NecDecoder {
public:
    Decoded decode(const Frame& frame, uint64_t timestamp_ms);
    void reset();
private:
    Decoded previous_{};
    uint64_t previous_ms_ = 0;
};

// Portable, bounded little-endian storage with version and CRC; no struct ABI.
size_t serialize(const Frame& frame, uint8_t* output, size_t capacity);
bool deserialize(const uint8_t* data, size_t length, Frame& output);

} // namespace infrared::protocol
