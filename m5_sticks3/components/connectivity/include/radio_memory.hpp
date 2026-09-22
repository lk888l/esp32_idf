#pragma once

#include <array>
#include <cstring>
#include "connectivity_policy.hpp"

namespace connectivity {

inline constexpr uint8_t kMemorySlots = 4;
inline constexpr uint8_t kNoSlot = 255;
struct BlePeer { char address[18]{}; uint8_t address_type = 0; };

inline bool normalize_peer(BlePeer& peer)
{
    if (detail::bounded_length(peer.address) != 17 ||
        !valid_ble_address(peer.address) || peer.address_type > 3) return false;
    for (char& c : peer.address) if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
    return true;
}
inline bool same_peer(BlePeer left, BlePeer right)
{
    return normalize_peer(left) && normalize_peer(right) &&
        (left.address_type & 1) == (right.address_type & 1) &&
        std::strcmp(left.address, right.address) == 0;
}
// Private advertising addresses expire; remember only stable/resolved identities.
inline bool stable_peer(BlePeer peer)
{
    if (!normalize_peer(peer)) return false;
    return (peer.address_type & 1) == 0 || peer.address[0] >= 'C';
}

struct RadioMemory {
    std::array<WifiCredentials, kMemorySlots> wifi{};
    std::array<BlePeer, kMemorySlots> ble{};
    uint8_t preferred_wifi = kNoSlot;
    uint8_t preferred_ble = kNoSlot;
    uint8_t wifi_slot(const char* ssid) const
    {
        for (uint8_t i = 0; i < kMemorySlots; ++i)
            if (wifi[i].ssid[0] && std::strcmp(wifi[i].ssid, ssid) == 0) return i;
        for (uint8_t i = 0; i < kMemorySlots; ++i) if (!wifi[i].ssid[0]) return i;
        return kNoSlot;
    }
    uint8_t ble_slot(const BlePeer& peer) const
    {
        for (uint8_t i = 0; i < kMemorySlots; ++i) if (same_peer(ble[i], peer)) return i;
        for (uint8_t i = 0; i < kMemorySlots; ++i) if (!ble[i].address[0]) return i;
        return kNoSlot;
    }
    bool remember(const WifiCredentials& credentials)
    {
        if (!valid_credentials(credentials)) return false;
        const auto slot = wifi_slot(credentials.ssid);
        if (slot == kNoSlot) return false; // Never silently evict another network.
        wifi[slot] = credentials; preferred_wifi = slot; return true;
    }
    bool remember(BlePeer peer)
    {
        if (!normalize_peer(peer) || !stable_peer(peer)) return false;
        const auto slot = ble_slot(peer);
        if (slot == kNoSlot) return false;
        ble[slot] = peer; preferred_ble = slot; return true;
    }
    void forget_wifi(uint8_t slot)
    {
        if (slot == kNoSlot) { wifi = {}; preferred_wifi = kNoSlot; }
        else if (slot < kMemorySlots) {
            wifi[slot] = {};
            if (preferred_wifi == slot) preferred_wifi = kNoSlot;
        }
    }
    void forget_ble(uint8_t slot)
    {
        if (slot == kNoSlot) { ble = {}; preferred_ble = kNoSlot; }
        else if (slot < kMemorySlots) {
            ble[slot] = {};
            if (preferred_ble == slot) preferred_ble = kNoSlot;
        }
    }
};

// Padding-free, explicitly versioned NVS encoding. NVS supplies the blob CRC.
inline constexpr size_t kRadioMemoryBytes = 6 + kMemorySlots * (33 + 65 + 18 + 1);
using MemoryBytes = std::array<uint8_t, kRadioMemoryBytes>;
inline MemoryBytes encode_memory(const RadioMemory& value)
{
    MemoryBytes output{};
    std::memcpy(output.data(), "RM01", 4);
    output[4] = value.preferred_wifi; output[5] = value.preferred_ble;
    size_t offset = 6;
    for (const auto& row : value.wifi) {
        std::memcpy(output.data() + offset, row.ssid, sizeof(row.ssid)); offset += sizeof(row.ssid);
        std::memcpy(output.data() + offset, row.password, sizeof(row.password)); offset += sizeof(row.password);
    }
    for (const auto& row : value.ble) {
        std::memcpy(output.data() + offset, row.address, sizeof(row.address)); offset += sizeof(row.address);
        output[offset++] = row.address_type;
    }
    return output;
}
inline bool decode_memory(const uint8_t* input, size_t size, RadioMemory& output)
{
    if (!input || size != kRadioMemoryBytes || std::memcmp(input, "RM01", 4) != 0) return false;
    RadioMemory value{};
    value.preferred_wifi = input[4]; value.preferred_ble = input[5];
    size_t offset = 6;
    for (auto& row : value.wifi) {
        std::memcpy(row.ssid, input + offset, sizeof(row.ssid)); offset += sizeof(row.ssid);
        std::memcpy(row.password, input + offset, sizeof(row.password)); offset += sizeof(row.password);
        if (row.ssid[0] ? !valid_credentials(row) : row.password[0] != 0) return false;
    }
    for (auto& row : value.ble) {
        std::memcpy(row.address, input + offset, sizeof(row.address)); offset += sizeof(row.address);
        row.address_type = input[offset++];
        if (row.address[0] && (!normalize_peer(row) || !stable_peer(row))) return false;
    }
    if (value.preferred_wifi != kNoSlot &&
        (value.preferred_wifi >= kMemorySlots || !value.wifi[value.preferred_wifi].ssid[0])) return false;
    if (value.preferred_ble != kNoSlot &&
        (value.preferred_ble >= kMemorySlots || !value.ble[value.preferred_ble].address[0])) return false;
    for (size_t i = 0; i < kMemorySlots; ++i) for (size_t j = i + 1; j < kMemorySlots; ++j) {
        if (value.wifi[i].ssid[0] && std::strcmp(value.wifi[i].ssid, value.wifi[j].ssid) == 0) return false;
        if (value.ble[i].address[0] && same_peer(value.ble[i], value.ble[j])) return false;
    }
    output = value; // Corrupt input never partially replaces live state.
    return true;
}
// Published copy deliberately contains no Wi-Fi passwords or BLE keys.
struct MemorySnapshot {
    char wifi_ssids[kMemorySlots][33]{};
    BlePeer ble[kMemorySlots]{};
    uint8_t preferred_wifi = kNoSlot, preferred_ble = kNoSlot;
    bool wifi_pending = false, ble_pending = false;
    int32_t wifi_error = 0, ble_error = 0;
};
} // namespace connectivity
