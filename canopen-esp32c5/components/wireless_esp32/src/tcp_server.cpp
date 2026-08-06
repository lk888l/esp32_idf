#include "wireless_esp32/tcp_server.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace wireless_esp32 {
namespace {
constexpr char kTag[] = "wireless_tcp";
constexpr char kDiscoveryRequest[] = "HXDISC1";
constexpr uint32_t kCapabilities = 0x0000003FU;

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000U);
}

void put_u16(uint8_t* output, uint16_t value)
{
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
}

void put_u32(uint8_t* output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

bool constant_time_equal(std::span<const uint8_t> left, std::span<const uint8_t> right)
{
    if (left.size() != right.size()) {
        return false;
    }
    uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

bool set_nonblocking(int socket)
{
    const int current = fcntl(socket, F_GETFL, 0);
    return current >= 0 && fcntl(socket, F_SETFL, current | O_NONBLOCK) == 0;
}

} // namespace

TcpServer::TcpServer(TcpServerConfig config,
                     const SettingsStore& settings_store,
                     PacketSink& packet_sink)
    : Task("wireless_tcp", config.task_stack_size, config.task_priority)
    , config_(config)
    , settings_store_(settings_store)
    , packet_sink_(packet_sink)
{
    for (auto& client : clients_) {
        client.socket = -1;
    }
}

TcpServer::~TcpServer()
{
    (void)stop(pdMS_TO_TICKS(2000));
    close_sockets();
}

bool TcpServer::initialize()
{
    if (outbound_queue_ != nullptr || config_.port == 0 || config_.discovery_port == 0 ||
        config_.max_clients == 0 || config_.max_clients > kMaxClients ||
        !settings_store_.ready()) {
        return false;
    }
    outbound_queue_ = xQueueCreateStatic(kOutboundDepth,
                                         sizeof(OutboundPacket),
                                         outbound_storage_.data(),
                                         &outbound_queue_control_);
    return outbound_queue_ != nullptr;
}

bool TcpServer::enqueue(uint8_t target_peer, const wireless::Packet& packet)
{
    if (outbound_queue_ == nullptr || !packet.valid() ||
        (target_peer != kBroadcastPeer && target_peer >= config_.max_clients)) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const OutboundPacket outbound{.target_peer = target_peer, .packet = packet};
    if (xQueueSend(outbound_queue_, &outbound, 0) != pdTRUE) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

TcpServerStatistics TcpServer::statistics() const
{
    return {
        .accepted = accepted_.load(std::memory_order_relaxed),
        .authenticated = authenticated_.load(std::memory_order_relaxed),
        .rejected = rejected_.load(std::memory_order_relaxed),
        .rx_packets = rx_packets_.load(std::memory_order_relaxed),
        .tx_packets = tx_packets_.load(std::memory_order_relaxed),
        .rx_invalid = rx_invalid_.load(std::memory_order_relaxed),
        .tx_dropped = tx_dropped_.load(std::memory_order_relaxed),
    };
}

void TcpServer::run()
{
    if (!open_sockets()) {
        ESP_LOGE(kTag, "could not open TCP/UDP sockets");
        return;
    }
    ESP_LOGI(kTag,
             "CAN-FD tunnel listening on TCP %u, discovery UDP %u",
             config_.port,
             config_.discovery_port);

    while (!stop_requested()) {
        drain_outbound();

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(listen_socket_, &read_set);
        FD_SET(discovery_socket_, &read_set);
        int maximum = std::max(listen_socket_, discovery_socket_);
        for (std::size_t index = 0; index < config_.max_clients; ++index) {
            if (clients_[index].socket < 0) {
                continue;
            }
            if (!clients_[index].authenticated &&
                now_ms() - clients_[index].connected_at_ms > 10000U) {
                close_client(index);
                continue;
            }
            FD_SET(clients_[index].socket, &read_set);
            if (clients_[index].tx_count != 0) {
                FD_SET(clients_[index].socket, &write_set);
            }
            maximum = std::max(maximum, clients_[index].socket);
        }

        timeval timeout{.tv_sec = 0, .tv_usec = 10000};
        const int ready = select(maximum + 1, &read_set, &write_set, nullptr, &timeout);
        if (ready < 0) {
            if (errno != EINTR) {
                ESP_LOGE(kTag, "select failed: errno=%d", errno);
                break;
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }
        if (FD_ISSET(listen_socket_, &read_set)) {
            accept_client();
        }
        if (FD_ISSET(discovery_socket_, &read_set)) {
            receive_discovery();
        }
        for (std::size_t index = 0; index < config_.max_clients; ++index) {
            if (clients_[index].socket < 0) {
                continue;
            }
            const int client_socket = clients_[index].socket;
            if (FD_ISSET(client_socket, &read_set) && !receive_client(index)) {
                close_client(index);
                continue;
            }
            if (clients_[index].socket >= 0 &&
                FD_ISSET(client_socket, &write_set) &&
                !flush_client(index)) {
                close_client(index);
                continue;
            }
            if (clients_[index].socket >= 0 &&
                clients_[index].close_after_tx &&
                clients_[index].tx_count == 0) {
                close_client(index);
            }
        }
    }
    close_sockets();
}

bool TcpServer::open_sockets()
{
    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    discovery_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (listen_socket_ < 0 || discovery_socket_ < 0) {
        close_sockets();
        return false;
    }
    const int enable = 1;
    (void)setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    (void)setsockopt(discovery_socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in tcp_address{};
    tcp_address.sin_family = AF_INET;
    tcp_address.sin_port = htons(config_.port);
    tcp_address.sin_addr.s_addr = htonl(INADDR_ANY);
    sockaddr_in udp_address{};
    udp_address.sin_family = AF_INET;
    udp_address.sin_port = htons(config_.discovery_port);
    udp_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_socket_,
             reinterpret_cast<const sockaddr*>(&tcp_address),
             sizeof(tcp_address)) != 0 ||
        listen(listen_socket_, config_.max_clients) != 0 ||
        bind(discovery_socket_,
             reinterpret_cast<const sockaddr*>(&udp_address),
             sizeof(udp_address)) != 0 ||
        !set_nonblocking(listen_socket_) || !set_nonblocking(discovery_socket_)) {
        close_sockets();
        return false;
    }
    return true;
}

void TcpServer::close_sockets()
{
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        close_client(index);
    }
    if (listen_socket_ >= 0) {
        close(listen_socket_);
        listen_socket_ = -1;
    }
    if (discovery_socket_ >= 0) {
        close(discovery_socket_);
        discovery_socket_ = -1;
    }
}

void TcpServer::accept_client()
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const int accepted_socket =
        accept(listen_socket_, reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted_socket < 0) {
        return;
    }

    std::size_t slot = clients_.size();
    for (std::size_t index = 0; index < config_.max_clients; ++index) {
        if (clients_[index].socket < 0) {
            slot = index;
            break;
        }
    }
    if (slot == clients_.size() || !set_nonblocking(accepted_socket)) {
        close(accepted_socket);
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const int enable = 1;
    (void)setsockopt(accepted_socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    (void)setsockopt(accepted_socket, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
    clients_[slot] = {};
    clients_[slot].socket = accepted_socket;
    clients_[slot].connected_at_ms = now_ms();
    accepted_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(kTag, "client %u connected; authentication required", slot);
}

void TcpServer::receive_discovery()
{
    std::array<uint8_t, 64> request{};
    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const int received = recvfrom(discovery_socket_,
                                  request.data(),
                                  request.size(),
                                  0,
                                  reinterpret_cast<sockaddr*>(&source),
                                  &source_length);
    if (received != static_cast<int>(sizeof(kDiscoveryRequest) - 1) ||
        std::memcmp(request.data(), kDiscoveryRequest, sizeof(kDiscoveryRequest) - 1) != 0) {
        return;
    }

    std::array<uint8_t, 24> reply{};
    std::copy_n(kDiscoveryRequest, sizeof(kDiscoveryRequest) - 1, reply.begin());
    reply[7] = wireless::kProtocolVersion;
    put_u16(reply.data() + 8, config_.port);
    put_u32(reply.data() + 10, kCapabilities);
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return;
    }
    std::copy_n(mac, sizeof(mac), reply.begin() + 14);
    put_u32(reply.data() + 20, wireless::crc32_ieee(std::span<const uint8_t>(reply.data(), 20)));
    (void)sendto(discovery_socket_,
                 reply.data(),
                 reply.size(),
                 0,
                 reinterpret_cast<const sockaddr*>(&source),
                 source_length);
}

bool TcpServer::receive_client(std::size_t index)
{
    std::array<uint8_t, 256> bytes{};
    const int received = recv(clients_[index].socket, bytes.data(), bytes.size(), 0);
    if (received <= 0) {
        return received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    if (!clients_[index].decoder.append(
            std::span<const uint8_t>(bytes.data(), static_cast<std::size_t>(received)))) {
        rx_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    wireless::Packet packet{};
    while (true) {
        const wireless::DecodeStatus status = clients_[index].decoder.next(packet);
        if (status == wireless::DecodeStatus::incomplete) {
            return true;
        }
        if (status == wireless::DecodeStatus::invalid) {
            rx_invalid_.fetch_add(1, std::memory_order_relaxed);
            if (++clients_[index].invalid_packets >= 8) {
                return false;
            }
            continue;
        }
        clients_[index].invalid_packets = 0;
        rx_packets_.fetch_add(1, std::memory_order_relaxed);
        if (!handle_packet(index, packet)) {
            return false;
        }
    }
}

bool TcpServer::handle_packet(std::size_t index, const wireless::Packet& packet)
{
    if (packet.type == wireless::MessageType::auth_request) {
        return handle_auth_request(index, packet);
    }
    if (packet.type == wireless::MessageType::auth_response) {
        return handle_auth_response(index, packet);
    }
    if (!clients_[index].authenticated) {
        wireless::Packet response{};
        response.type = wireless::MessageType::auth_result;
        response.flags = wireless::packet_flag_response | wireless::packet_flag_error;
        response.sequence = packet.sequence;
        response.payload_size = 1;
        response.payload[0] = 0;
        return send_packet(index, response);
    }
    return packet_sink_.submit(
        {.link = LinkKind::tcp, .peer = static_cast<uint8_t>(index), .packet = packet});
}

bool TcpServer::handle_auth_request(std::size_t index, const wireless::Packet& packet)
{
    if (packet.payload_size != kNonceSize) {
        return false;
    }
    Client& client = clients_[index];
    std::copy_n(packet.payload.begin(), kNonceSize, client.client_nonce.begin());
    esp_fill_random(client.device_nonce.data(), client.device_nonce.size());

    wireless::Packet response{};
    response.type = wireless::MessageType::auth_challenge;
    response.flags = wireless::packet_flag_response;
    response.sequence = packet.sequence;
    response.payload_size = 64;
    std::copy(client.client_nonce.begin(), client.client_nonce.end(), response.payload.begin());
    std::copy(client.device_nonce.begin(),
              client.device_nonce.end(),
              response.payload.begin() + kNonceSize);
    std::array<uint8_t, 32> proof{};
    if (!calculate_hmac("HX-AUTH-DEVICE-v1",
                        client.client_nonce,
                        client.device_nonce,
                        proof)) {
        return false;
    }
    std::copy(proof.begin(), proof.end(), response.payload.begin() + 2 * kNonceSize);
    client.challenge_pending = true;
    client.authenticated = false;
    return send_packet(index, response);
}

bool TcpServer::handle_auth_response(std::size_t index, const wireless::Packet& packet)
{
    Client& client = clients_[index];
    if (!client.challenge_pending || packet.payload_size != 32) {
        return false;
    }
    std::array<uint8_t, 32> expected{};
    if (!calculate_hmac("HX-AUTH-CLIENT-v1",
                        client.client_nonce,
                        client.device_nonce,
                        expected)) {
        return false;
    }
    const bool accepted =
        constant_time_equal(std::span<const uint8_t>(packet.payload.data(), packet.payload_size),
                            expected);
    client.challenge_pending = false;
    client.authenticated = accepted;

    wireless::Packet response{};
    response.type = wireless::MessageType::auth_result;
    response.flags = wireless::packet_flag_response |
                     (accepted ? wireless::packet_flag_none : wireless::packet_flag_error);
    response.sequence = packet.sequence;
    response.payload_size = 1;
    response.payload[0] = accepted ? 1 : 0;
    const bool sent = send_packet(index, response);
    if (accepted) {
        authenticated_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGI(kTag, "client %u authenticated", index);
    } else {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        client.close_after_tx = true;
    }
    return sent;
}

bool TcpServer::send_packet(std::size_t index, const wireless::Packet& packet)
{
    if (index >= config_.max_clients || clients_[index].socket < 0) {
        return false;
    }
    Client& client = clients_[index];
    if (client.tx_count >= kClientTxDepth) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    wireless::WirePacket wire{};
    if (!wireless::encode_packet(packet, wire)) {
        return false;
    }
    const std::size_t tail =
        (static_cast<std::size_t>(client.tx_head) + client.tx_count) % kClientTxDepth;
    client.tx_queue[tail] = wire;
    ++client.tx_count;
    return true;
}

bool TcpServer::flush_client(std::size_t index)
{
    if (index >= config_.max_clients || clients_[index].socket < 0) {
        return false;
    }
    Client& client = clients_[index];
    unsigned flushed = 0;
    while (client.tx_count != 0 && flushed < kClientTxDepth) {
        wireless::WirePacket& wire = client.tx_queue[client.tx_head];
        const std::size_t remaining = wire.size - client.tx_offset;
        const int sent = send(client.socket,
                              wire.bytes.data() + client.tx_offset,
                              remaining,
                              MSG_DONTWAIT);
        if (sent > 0) {
            client.tx_offset += static_cast<uint16_t>(sent);
            if (client.tx_offset < wire.size) {
                continue;
            }
            client.tx_offset = 0;
            client.tx_head =
                static_cast<uint8_t>((client.tx_head + 1U) % kClientTxDepth);
            --client.tx_count;
            ++flushed;
            tx_packets_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    return true;
}

void TcpServer::drain_outbound()
{
    OutboundPacket outbound{};
    while (outbound_queue_ != nullptr &&
           xQueueReceive(outbound_queue_, &outbound, 0) == pdTRUE) {
        if (outbound.target_peer == kBroadcastPeer) {
            for (std::size_t index = 0; index < config_.max_clients; ++index) {
                if (clients_[index].authenticated &&
                    !send_packet(index, outbound.packet)) {
                    close_client(index);
                }
            }
        } else if (clients_[outbound.target_peer].authenticated &&
                   !send_packet(outbound.target_peer, outbound.packet)) {
            close_client(outbound.target_peer);
        }
    }
}

void TcpServer::close_client(std::size_t index)
{
    if (index >= clients_.size()) {
        return;
    }
    if (clients_[index].socket >= 0) {
        shutdown(clients_[index].socket, SHUT_RDWR);
        close(clients_[index].socket);
    }
    clients_[index] = {};
    clients_[index].socket = -1;
}

bool TcpServer::calculate_hmac(
    const char* label,
    const std::array<uint8_t, kNonceSize>& client_nonce,
    const std::array<uint8_t, kNonceSize>& device_nonce,
    std::array<uint8_t, 32>& output) const
{
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        return false;
    }
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const auto& key = settings_store_.settings().control_key;
    bool success = mbedtls_md_setup(&context, info, 1) == 0 &&
                   mbedtls_md_hmac_starts(&context, key.data(), key.size()) == 0 &&
                   mbedtls_md_hmac_update(
                       &context,
                       reinterpret_cast<const unsigned char*>(label),
                       std::strlen(label)) == 0 &&
                   mbedtls_md_hmac_update(
                       &context, client_nonce.data(), client_nonce.size()) == 0 &&
                   mbedtls_md_hmac_update(
                       &context, device_nonce.data(), device_nonce.size()) == 0 &&
                   mbedtls_md_hmac_finish(&context, output.data()) == 0;
    mbedtls_md_free(&context);
    return success;
}

} // namespace wireless_esp32
