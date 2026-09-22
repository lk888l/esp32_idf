#pragma once

#include <cstddef>
#include <cstdint>
#include "radio_memory.hpp"

namespace connectivity {

constexpr size_t kMaxBleDevices = 16;
constexpr size_t kMaxBleServices = 8;
constexpr size_t kMaxBleBonds = 8;

// Bounded copies are safe to render after releasing the transport state lock.
struct BleDevice {
    char name[32]{};
    char address[18]{};
    int8_t rssi = 0;
    uint8_t address_type = 0;
    bool connectable = false;
};

struct BleDiagnostics {
    bool enabled = false;
    bool scanning = false;
    bool connecting = false;
    bool peer_connected = false;
    bool switching = false;
    bool peer_encrypted = false, peer_bonded = false;
    uint8_t peer_address_type = 0, peer_identity_type = 0;
    char peer_identity[18]{};
    uint32_t peer_generation = 0;
    int8_t peer_rssi = 0;
    uint16_t peer_mtu = 23;
    char peer_address[18]{};
    char peer_name[32]{};
    uint8_t count = 0;
    uint16_t total_found = 0;
    uint32_t generation = 0;
    int32_t scan_error = 0;
    int32_t peer_error = 0;
    uint8_t service_count = 0;
    char services[kMaxBleServices][40]{};
    BleDevice devices[kMaxBleDevices]{};
    uint8_t bond_count = 0;
    int32_t bond_error = 0;
    uint32_t bond_generation = 0;
    BlePeer bonds[kMaxBleBonds]{};
};

} // namespace connectivity
