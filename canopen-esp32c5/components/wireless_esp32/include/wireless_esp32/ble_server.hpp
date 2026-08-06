#pragma once

#include "wireless_esp32/packet_transport.hpp"
#include "wireless_esp32/settings_store.hpp"

#include "host/ble_gatt.h"

#include <array>
#include <atomic>
#include <cstdint>

struct ble_gap_event;

namespace wireless_esp32 {

struct BleServerConfig {
    std::array<char, 32> device_name{};
};

struct BleServerStatistics {
    uint32_t connections = 0;
    uint32_t rx_packets = 0;
    uint32_t rx_invalid = 0;
    uint32_t tx_packets = 0;
    uint32_t tx_dropped = 0;
};

class BleServer {
public:
    BleServer(BleServerConfig config,
              const SettingsStore& settings_store,
              PacketSink& packet_sink);
    ~BleServer();

    bool initialize();
    bool deinitialize();
    bool notify(const wireless::Packet& packet);
    [[nodiscard]] bool connected() const
    {
        return connection_handle_.load(std::memory_order_relaxed) != 0xFFFFU;
    }
    [[nodiscard]] BleServerStatistics statistics() const;

private:
    static int gap_event(ble_gap_event* event, void* argument);
    static int gatt_access(uint16_t connection_handle,
                           uint16_t attribute_handle,
                           ble_gatt_access_ctxt* context,
                           void* argument);
    static void on_sync();
    static void on_reset(int reason);
    static void host_task(void* argument);

    int handle_gap_event(ble_gap_event* event);
    int handle_gatt_access(uint16_t connection_handle,
                           uint16_t attribute_handle,
                           ble_gatt_access_ctxt* context);
    void advertise();

    static BleServer* instance_;
    BleServerConfig config_;
    const SettingsStore& settings_store_;
    PacketSink& packet_sink_;
    std::array<ble_gatt_chr_def, 3> characteristics_{};
    std::array<ble_gatt_svc_def, 2> services_{};
    uint16_t rx_value_handle_ = 0;
    uint16_t tx_value_handle_ = 0;
    uint8_t own_address_type_ = 0;
    std::atomic<uint16_t> connection_handle_{0xFFFFU};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint32_t> connections_{0};
    std::atomic<uint32_t> rx_packets_{0};
    std::atomic<uint32_t> rx_invalid_{0};
    std::atomic<uint32_t> tx_packets_{0};
    std::atomic<uint32_t> tx_dropped_{0};
};

} // namespace wireless_esp32
