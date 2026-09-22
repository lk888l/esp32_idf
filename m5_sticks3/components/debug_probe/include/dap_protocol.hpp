#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace debug_probe {
inline constexpr size_t kPacketSize = 64;
inline constexpr uint16_t kTcpPort = 4441;
inline constexpr unsigned kSwclk = 6, kSwdio = 7, kReset = 8;
inline constexpr std::array<uint32_t, 5> kClockPresets = {100000, 250000, 500000, 1000000, 2000000};
struct Packet { uint16_t size = 0; uint8_t data[kPacketSize]{}; };
inline uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void write16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
inline void write32(uint8_t* p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8*i); }

// Arm's reference engine assumes trusted, padded packets. Validate both the
// consumed request and worst-case response before passing it any host bytes.
inline bool valid_request(const uint8_t* p, size_t n) {
    if (!p || n == 0 || n > kPacketSize) return false;
    size_t need = 1, output = 1;
    switch (p[0]) {
    case 0x00: need = 2; break; // Info
    case 0x01: need = 3; break;
    case 0x02: need = 2; break;
    case 0x03: case 0x07: case 0x0a: break;
    case 0x04: need = 6; break;
    case 0x08: need = 6; break;
    case 0x09: need = 3; break;
    case 0x10: need = 7; break;
    case 0x11: need = 5; break;
    case 0x13: need = 2; break;
    case 0x12:
        if (n < 2) return false;
        need = 2 + ((p[1] ? unsigned(p[1]) : 256U) + 7) / 8;
        break;
    case 0x05: {
        if (n < 3) return false;
        need = 3; output = 3;
        for (unsigned i = 0; i < p[2]; ++i) {
            if (need >= n) return false;
            const uint8_t flags = p[need++];
            if (flags & 0x40) return false;
            if (flags & 2) {
                if (flags & 0x20) return false;
                if (flags & 0x10) need += 4;
                else output += 4 + ((flags & 0x80) ? 4 : 0);
            } else {
                if (flags & 0x10) return false;
                need += 4;
                if ((flags & 0x80) && !(flags & 0x20)) output += 4;
            }
            if (need > n || output > kPacketSize) return false;
        }
        break;
    }
    case 0x06: {
        if (n < 5 || (p[4] & 0xf0)) return false;
        const size_t count = read16(p + 2);
        need = 5 + ((p[4] & 2) ? 0 : 4 * count);
        output = 4 + ((p[4] & 2) ? 4 * count : 0);
        break;
    }
    case 0x1d:
        if (n < 2) return false;
        need = 2; output = 2;
        for (unsigned i = 0; i < p[1]; ++i) {
            if (need >= n) return false;
            const uint8_t info = p[need++];
            if (info & 0x40) return false;
            const size_t bytes = (((info & 63) ? (info & 63) : 64) + 7) / 8;
            if (info & 0x80) output += bytes; else need += bytes;
            if (need > n || output > kPacketSize) return false;
        }
        break;
    // Unknown and unsupported commands are answered with ID_DAP_Invalid by the
    // wrapper. In particular, do not dispatch unchecked atomic/vendor commands.
    default: return false;
    }
    return need <= n && output <= kPacketSize;
}

inline bool tcp_length(const uint8_t* h, uint16_t& n) {
    n = read16(h + 4);
    return read32(h) == 0x00504144 && h[6] == 1 && h[7] == 0 && n > 0 && n <= kPacketSize;
}
inline void tcp_header(uint8_t* h, uint16_t n, uint8_t type = 2) {
    write32(h, 0x00504144); write16(h + 4, n); h[6] = type; h[7] = 0;
}

// BLE frames: sequence:u16, offset:u8, total:u8, payload (1..MTU-7).
// Only contiguous fragments of one packet are accepted. Offset zero explicitly
// begins a new packet. At MTU=23, each write carries up to 16 DAP bytes.
class BleAssembler {
public:
    enum class Result { invalid, partial, complete };
    Result append(const uint8_t* p, size_t n) {
        if (!p || n < 5 || n > kPacketSize + 4 || !p[3] || p[3] > kPacketSize) return reject();
        const uint16_t id = read16(p);
        if (p[2] == 0) { reset(); sequence = id; packet.size = p[3]; }
        if (!packet.size || id != sequence || p[3] != packet.size || p[2] != used || n - 4 > packet.size - used)
            return reject();
        std::memcpy(packet.data + used, p + 4, n - 4);
        used += n - 4;
        return used == packet.size ? Result::complete : Result::partial;
    }
    void reset() { used = 0; packet = {}; sequence = 0; }
    Packet packet{};
    uint16_t sequence = 0;
private:
    Result reject() { reset(); return Result::invalid; }
    size_t used = 0;
};
} // namespace debug_probe
