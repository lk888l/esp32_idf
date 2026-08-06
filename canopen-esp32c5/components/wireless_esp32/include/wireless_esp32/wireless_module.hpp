#pragma once

#include "app/app_module.hpp"
#include "app/app_task.hpp"
#include "canopen_esp32/esp_can_gateway.hpp"
#include "wireless_esp32/ble_server.hpp"
#include "wireless_esp32/packet_transport.hpp"
#include "wireless_esp32/settings_store.hpp"
#include "wireless_esp32/tcp_server.hpp"
#include "wireless_esp32/wifi_manager.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wireless_esp32 {

struct WirelessModuleConfig {
    WifiManagerConfig wifi{};
    TcpServerConfig tcp{};
    BleServerConfig ble{};
    uint8_t canopen_node_id = 0x21;
    uint32_t ble_passkey = 0;
    std::array<char, WirelessSettings::kPasswordCapacity> ap_password{};
    std::array<uint8_t, WirelessSettings::kControlKeySize> control_key{};
    std::array<char, 16> firmware_version{};
    uint32_t service_task_stack_size = 6144;
    UBaseType_t service_task_priority = 9;
};

struct WirelessModuleStatistics {
    uint32_t ingress_packets = 0;
    uint32_t ingress_dropped = 0;
    uint32_t can_injected = 0;
    uint32_t can_rejected = 0;
    uint32_t protocol_errors = 0;
};

class WirelessModule final : public app::Module, public PacketSink {
public:
    static constexpr std::size_t kIngressDepth = 64;

    WirelessModule(WirelessModuleConfig config,
                   canopen_esp32::EspCanGateway& can_gateway);
    ~WirelessModule() override;

    bool initialize() override;
    bool deinitialize() override;
    void process() override;
    [[nodiscard]] bool initialized() const override { return initialized_; }
    [[nodiscard]] std::string_view name() const override { return "wireless"; }
    bool submit(const InboundPacket& packet) override;
    [[nodiscard]] WirelessModuleStatistics statistics() const;

private:
    class ServiceTask final : public app::Task {
    public:
        ServiceTask(WirelessModule& owner, uint32_t stack_size, UBaseType_t priority)
            : Task("wireless_gateway", stack_size, priority), owner_(owner)
        {
        }

    private:
        void run() override;
        WirelessModule& owner_;
    };

    void handle_inbound(const InboundPacket& inbound);
    void forward_can_frames();
    void send_reply(const InboundPacket& inbound, const wireless::Packet& reply);
    void broadcast(const wireless::Packet& packet);
    wireless::Packet make_device_info(uint32_t sequence) const;
    wireless::Packet make_status(uint32_t sequence) const;
    wireless::Packet make_error(uint32_t sequence,
                                wireless::MessageType offending_type,
                                uint8_t error_code) const;
    void handle_wifi_credentials(const InboundPacket& inbound);

    WirelessModuleConfig config_;
    canopen_esp32::EspCanGateway& can_gateway_;
    SettingsStore settings_store_;
    WifiManager wifi_manager_;
    TcpServer tcp_server_;
    BleServer ble_server_;
    ServiceTask service_task_;
    StaticQueue_t ingress_queue_control_{};
    std::array<uint8_t, kIngressDepth * sizeof(InboundPacket)> ingress_storage_{};
    QueueHandle_t ingress_queue_ = nullptr;
    bool initialized_ = false;
    uint32_t last_report_ms_ = 0;
    std::atomic<uint32_t> sequence_{1};
    std::atomic<uint32_t> ingress_packets_{0};
    std::atomic<uint32_t> ingress_dropped_{0};
    std::atomic<uint32_t> can_injected_{0};
    std::atomic<uint32_t> can_rejected_{0};
    std::atomic<uint32_t> protocol_errors_{0};
};

} // namespace wireless_esp32
