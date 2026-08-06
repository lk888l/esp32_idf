#pragma once

#include "app/app_task.hpp"
#include "wireless_esp32/packet_transport.hpp"
#include "wireless_esp32/settings_store.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wireless_esp32 {

struct TcpServerConfig {
    uint16_t port = 3333;
    uint16_t discovery_port = 3334;
    uint8_t max_clients = 2;
    uint32_t task_stack_size = 6144;
    UBaseType_t task_priority = 8;
};

struct TcpServerStatistics {
    uint32_t accepted = 0;
    uint32_t authenticated = 0;
    uint32_t rejected = 0;
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t rx_invalid = 0;
    uint32_t tx_dropped = 0;
};

class TcpServer final : public app::Task {
public:
    static constexpr std::size_t kMaxClients = 4;
    static constexpr std::size_t kOutboundDepth = 64;
    static constexpr std::size_t kClientTxDepth = 8;
    static constexpr std::size_t kNonceSize = 16;

    TcpServer(TcpServerConfig config,
              const SettingsStore& settings_store,
              PacketSink& packet_sink);
    ~TcpServer() override;

    bool initialize();
    bool enqueue(uint8_t target_peer, const wireless::Packet& packet);
    [[nodiscard]] TcpServerStatistics statistics() const;

private:
    struct Client {
        int socket = -1;
        bool authenticated = false;
        bool challenge_pending = false;
        uint8_t invalid_packets = 0;
        uint32_t connected_at_ms = 0;
        wireless::StreamDecoder decoder{};
        std::array<uint8_t, kNonceSize> client_nonce{};
        std::array<uint8_t, kNonceSize> device_nonce{};
        std::array<wireless::WirePacket, kClientTxDepth> tx_queue{};
        uint8_t tx_head = 0;
        uint8_t tx_count = 0;
        uint16_t tx_offset = 0;
        bool close_after_tx = false;
    };

    struct OutboundPacket {
        uint8_t target_peer = kBroadcastPeer;
        wireless::Packet packet{};
    };

    void run() override;
    bool open_sockets();
    void close_sockets();
    void accept_client();
    void receive_discovery();
    bool receive_client(std::size_t index);
    bool handle_packet(std::size_t index, const wireless::Packet& packet);
    bool handle_auth_request(std::size_t index, const wireless::Packet& packet);
    bool handle_auth_response(std::size_t index, const wireless::Packet& packet);
    bool send_packet(std::size_t index, const wireless::Packet& packet);
    bool flush_client(std::size_t index);
    void drain_outbound();
    void close_client(std::size_t index);
    bool calculate_hmac(const char* label,
                        const std::array<uint8_t, kNonceSize>& client_nonce,
                        const std::array<uint8_t, kNonceSize>& device_nonce,
                        std::array<uint8_t, 32>& output) const;

    TcpServerConfig config_;
    const SettingsStore& settings_store_;
    PacketSink& packet_sink_;
    std::array<Client, kMaxClients> clients_{};
    int listen_socket_ = -1;
    int discovery_socket_ = -1;
    StaticQueue_t outbound_queue_control_{};
    std::array<uint8_t, kOutboundDepth * sizeof(OutboundPacket)> outbound_storage_{};
    QueueHandle_t outbound_queue_ = nullptr;
    std::atomic<uint32_t> accepted_{0};
    std::atomic<uint32_t> authenticated_{0};
    std::atomic<uint32_t> rejected_{0};
    std::atomic<uint32_t> rx_packets_{0};
    std::atomic<uint32_t> tx_packets_{0};
    std::atomic<uint32_t> rx_invalid_{0};
    std::atomic<uint32_t> tx_dropped_{0};
};

} // namespace wireless_esp32
