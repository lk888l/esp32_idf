#include "wireless_esp32/wireless_module.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace wireless_esp32 {
namespace {
constexpr char kTag[] = "wireless";
constexpr uint32_t kCapabilities = 0x0000003FU;
constexpr uint8_t kErrorInvalidPacket = 1;
constexpr uint8_t kErrorForbidden = 2;
constexpr uint8_t kErrorQueueFull = 3;
constexpr uint8_t kErrorUnsupported = 4;

void put_u32(uint8_t* output, uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void append_u32(wireless::Packet& packet, uint32_t value)
{
    if (packet.payload_size + 4 > packet.payload.size()) {
        return;
    }
    put_u32(packet.payload.data() + packet.payload_size, value);
    packet.payload_size += 4;
}

wireless::FrameOrigin map_origin(canopen_esp32::GatewayOrigin origin)
{
    switch (origin) {
    case canopen_esp32::GatewayOrigin::physical_bus:
        return wireless::FrameOrigin::physical_bus;
    case canopen_esp32::GatewayOrigin::local_node:
        return wireless::FrameOrigin::local_node;
    case canopen_esp32::GatewayOrigin::wireless_client:
        return wireless::FrameOrigin::wireless_client;
    }
    return wireless::FrameOrigin::physical_bus;
}

} // namespace

WirelessModule::WirelessModule(WirelessModuleConfig config,
                               canopen_esp32::EspCanGateway& can_gateway)
    : config_(config)
    , can_gateway_(can_gateway)
    , wifi_manager_(config_.wifi)
    , tcp_server_(config_.tcp, settings_store_, *this)
    , ble_server_(config_.ble, settings_store_, *this)
    , service_task_(*this,
                    config_.service_task_stack_size,
                    config_.service_task_priority)
{
}

WirelessModule::~WirelessModule()
{
    (void)deinitialize();
}

bool WirelessModule::initialize()
{
    if (initialized_) {
        return true;
    }
    ingress_queue_ = xQueueCreateStatic(kIngressDepth,
                                        sizeof(InboundPacket),
                                        ingress_storage_.data(),
                                        &ingress_queue_control_);
    if (ingress_queue_ == nullptr ||
        !settings_store_.initialize(
            config_.ble_passkey, config_.ap_password.data(), config_.control_key) ||
        !tcp_server_.initialize() ||
        !wifi_manager_.initialize(settings_store_.settings())) {
        (void)wifi_manager_.deinitialize();
        return false;
    }
    if (!ble_server_.initialize()) {
        (void)wifi_manager_.deinitialize();
        return false;
    }
    if (!tcp_server_.start()) {
        (void)ble_server_.deinitialize();
        (void)wifi_manager_.deinitialize();
        return false;
    }
    if (!service_task_.start()) {
        (void)tcp_server_.stop(pdMS_TO_TICKS(2000));
        (void)ble_server_.deinitialize();
        (void)wifi_manager_.deinitialize();
        return false;
    }
    initialized_ = true;
    ESP_LOGI(kTag,
             "wireless gateway ready: SoftAP=%s BLE=%s TCP=%u UDP discovery=%u",
             settings_store_.settings().ap_ssid.data(),
             config_.ble.device_name.data(),
             config_.tcp.port,
             config_.tcp.discovery_port);
    return true;
}

bool WirelessModule::deinitialize()
{
    if (!initialized_) {
        return true;
    }
    const bool service_stopped = service_task_.stop(pdMS_TO_TICKS(2000));
    const bool tcp_stopped = tcp_server_.stop(pdMS_TO_TICKS(2000));
    const bool ble_stopped = ble_server_.deinitialize();
    const bool wifi_stopped = wifi_manager_.deinitialize();
    initialized_ = !(service_stopped && tcp_stopped && ble_stopped && wifi_stopped);
    return !initialized_;
}

void WirelessModule::process()
{
    if (!initialized_) {
        return;
    }
    wifi_manager_.process();
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000U);
    if (now - last_report_ms_ < 10000U) {
        return;
    }
    last_report_ms_ = now;
    const WirelessModuleStatistics module = statistics();
    const TcpServerStatistics tcp = tcp_server_.statistics();
    const BleServerStatistics ble = ble_server_.statistics();
    ESP_LOGI(kTag,
             "wifi=%u ap-clients=%u ingress=%lu/%lu can=%lu/%lu proto-err=%lu tcp-auth=%lu rx/tx=%lu/%lu ble-conn=%lu rx/tx=%lu/%lu",
             static_cast<unsigned>(wifi_manager_.state()),
             wifi_manager_.ap_client_count(),
             static_cast<unsigned long>(module.ingress_packets),
             static_cast<unsigned long>(module.ingress_dropped),
             static_cast<unsigned long>(module.can_injected),
             static_cast<unsigned long>(module.can_rejected),
             static_cast<unsigned long>(module.protocol_errors),
             static_cast<unsigned long>(tcp.authenticated),
             static_cast<unsigned long>(tcp.rx_packets),
             static_cast<unsigned long>(tcp.tx_packets),
             static_cast<unsigned long>(ble.connections),
             static_cast<unsigned long>(ble.rx_packets),
             static_cast<unsigned long>(ble.tx_packets));
}

bool WirelessModule::submit(const InboundPacket& packet)
{
    if (ingress_queue_ == nullptr || !packet.packet.valid() ||
        xQueueSend(ingress_queue_, &packet, 0) != pdTRUE) {
        ingress_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ingress_packets_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

WirelessModuleStatistics WirelessModule::statistics() const
{
    return {
        .ingress_packets = ingress_packets_.load(std::memory_order_relaxed),
        .ingress_dropped = ingress_dropped_.load(std::memory_order_relaxed),
        .can_injected = can_injected_.load(std::memory_order_relaxed),
        .can_rejected = can_rejected_.load(std::memory_order_relaxed),
        .protocol_errors = protocol_errors_.load(std::memory_order_relaxed),
    };
}

void WirelessModule::ServiceTask::run()
{
    while (!stop_requested()) {
        owner_.forward_can_frames();
        InboundPacket inbound{};
        if (owner_.ingress_queue_ != nullptr &&
            xQueueReceive(owner_.ingress_queue_, &inbound, pdMS_TO_TICKS(2)) == pdTRUE) {
            owner_.handle_inbound(inbound);
            for (unsigned count = 0; count < 15 &&
                                     xQueueReceive(owner_.ingress_queue_, &inbound, 0) == pdTRUE;
                 ++count) {
                owner_.handle_inbound(inbound);
            }
        }
    }
}

void WirelessModule::handle_inbound(const InboundPacket& inbound)
{
    switch (inbound.packet.type) {
    case wireless::MessageType::can_frame: {
        can::Frame frame{};
        wireless::CanMetadata metadata{};
        if (!wireless::parse_can_packet(inbound.packet, frame, metadata) ||
            metadata.bus != 0) {
            protocol_errors_.fetch_add(1, std::memory_order_relaxed);
            send_reply(inbound,
                       make_error(inbound.packet.sequence,
                                  inbound.packet.type,
                                  kErrorInvalidPacket));
            return;
        }
        if (can_gateway_.inject(frame, 0)) {
            can_injected_.fetch_add(1, std::memory_order_relaxed);
        } else {
            can_rejected_.fetch_add(1, std::memory_order_relaxed);
            send_reply(inbound,
                       make_error(inbound.packet.sequence,
                                  inbound.packet.type,
                                  kErrorQueueFull));
        }
        return;
    }

    case wireless::MessageType::hello:
        send_reply(inbound, make_device_info(inbound.packet.sequence));
        return;

    case wireless::MessageType::status_request:
        send_reply(inbound, make_status(inbound.packet.sequence));
        return;

    case wireless::MessageType::wifi_credentials:
        handle_wifi_credentials(inbound);
        return;

    case wireless::MessageType::ping: {
        wireless::Packet response = inbound.packet;
        response.type = wireless::MessageType::pong;
        response.flags = wireless::packet_flag_response;
        send_reply(inbound, response);
        return;
    }

    default:
        protocol_errors_.fetch_add(1, std::memory_order_relaxed);
        send_reply(inbound,
                   make_error(inbound.packet.sequence,
                              inbound.packet.type,
                              kErrorUnsupported));
        return;
    }
}

void WirelessModule::forward_can_frames()
{
    canopen_esp32::GatewayFrame routed{};
    for (unsigned count = 0; count < 32 && can_gateway_.receive_forwarded(routed, 0);
         ++count) {
        wireless::Packet packet{};
        if (wireless::make_can_packet(
                routed.frame,
                {.origin = map_origin(routed.origin),
                 .bus = 0,
                 .timestamp_us = routed.timestamp_us},
                sequence_.fetch_add(1, std::memory_order_relaxed),
                packet)) {
            broadcast(packet);
        }
    }
}

void WirelessModule::send_reply(const InboundPacket& inbound,
                                const wireless::Packet& reply)
{
    if (inbound.link == LinkKind::tcp) {
        (void)tcp_server_.enqueue(inbound.peer, reply);
    } else {
        (void)ble_server_.notify(reply);
    }
}

void WirelessModule::broadcast(const wireless::Packet& packet)
{
    (void)tcp_server_.enqueue(kBroadcastPeer, packet);
    (void)ble_server_.notify(packet);
}

wireless::Packet WirelessModule::make_device_info(uint32_t sequence) const
{
    wireless::Packet packet{};
    packet.type = wireless::MessageType::device_info;
    packet.flags = wireless::packet_flag_response;
    packet.sequence = sequence;
    append_u32(packet, kCapabilities);
    append_u32(packet, static_cast<uint32_t>(esp_timer_get_time() / 1000U));
    packet.payload[packet.payload_size++] = config_.canopen_node_id;
    packet.payload[packet.payload_size++] = static_cast<uint8_t>(wifi_manager_.state());
    packet.payload[packet.payload_size++] = ble_server_.connected() ? 1 : 0;
    packet.payload[packet.payload_size++] = wireless::kProtocolVersion;
    append_u32(packet, wifi_manager_.station_ipv4());
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::copy_n(mac, sizeof(mac), packet.payload.begin() + packet.payload_size);
        packet.payload_size += sizeof(mac);
    }
    const std::size_t version_length =
        strnlen(config_.firmware_version.data(), config_.firmware_version.size());
    packet.payload[packet.payload_size++] = static_cast<uint8_t>(version_length);
    std::copy_n(config_.firmware_version.begin(),
                version_length,
                packet.payload.begin() + packet.payload_size);
    packet.payload_size += version_length;
    return packet;
}

wireless::Packet WirelessModule::make_status(uint32_t sequence) const
{
    wireless::Packet packet{};
    packet.type = wireless::MessageType::status_response;
    packet.flags = wireless::packet_flag_response;
    packet.sequence = sequence;

    const auto twai = can_gateway_.twai_statistics();
    const auto gateway = can_gateway_.gateway_statistics();
    const auto module = statistics();
    const auto tcp = tcp_server_.statistics();
    const auto ble = ble_server_.statistics();
    append_u32(packet, static_cast<uint32_t>(wifi_manager_.state()));
    append_u32(packet, wifi_manager_.station_ipv4());
    append_u32(packet, wifi_manager_.ap_client_count());
    append_u32(packet, twai.rx_frames);
    append_u32(packet, twai.rx_dropped);
    append_u32(packet, twai.tx_frames);
    append_u32(packet, twai.tx_failed);
    append_u32(packet, twai.bus_errors);
    append_u32(packet, twai.recoveries);
    append_u32(packet, gateway.ingress_frames);
    append_u32(packet, gateway.ingress_dropped);
    append_u32(packet, gateway.forwarded_frames);
    append_u32(packet, gateway.monitor_dropped);
    append_u32(packet, module.ingress_packets);
    append_u32(packet, module.ingress_dropped);
    append_u32(packet, module.protocol_errors);
    append_u32(packet, tcp.authenticated);
    append_u32(packet, tcp.rx_packets);
    append_u32(packet, tcp.tx_packets);
    append_u32(packet, tcp.rx_invalid);
    append_u32(packet, tcp.tx_dropped);
    append_u32(packet, ble.connections);
    append_u32(packet, ble.rx_packets);
    append_u32(packet, ble.tx_packets);
    append_u32(packet, ble.rx_invalid);
    append_u32(packet, ble.tx_dropped);
    return packet;
}

wireless::Packet WirelessModule::make_error(uint32_t sequence,
                                            wireless::MessageType offending_type,
                                            uint8_t error_code) const
{
    wireless::Packet packet{};
    packet.type = wireless::MessageType::error;
    packet.flags = wireless::packet_flag_response | wireless::packet_flag_error;
    packet.sequence = sequence;
    packet.payload_size = 2;
    packet.payload[0] = error_code;
    packet.payload[1] = static_cast<uint8_t>(offending_type);
    return packet;
}

void WirelessModule::handle_wifi_credentials(const InboundPacket& inbound)
{
    if (inbound.link != LinkKind::ble) {
        send_reply(inbound,
                   make_error(inbound.packet.sequence,
                              inbound.packet.type,
                              kErrorForbidden));
        return;
    }
    if (inbound.packet.payload_size < 2) {
        send_reply(inbound,
                   make_error(inbound.packet.sequence,
                              inbound.packet.type,
                              kErrorInvalidPacket));
        return;
    }
    const std::size_t ssid_length = inbound.packet.payload[0];
    const std::size_t password_length = inbound.packet.payload[1];
    if (ssid_length == 0 || ssid_length > 32 || password_length > 63 ||
        inbound.packet.payload_size != 2 + ssid_length + password_length) {
        send_reply(inbound,
                   make_error(inbound.packet.sequence,
                              inbound.packet.type,
                              kErrorInvalidPacket));
        return;
    }
    std::array<char, WirelessSettings::kSsidCapacity> ssid{};
    std::array<char, WirelessSettings::kPasswordCapacity> password{};
    std::copy_n(inbound.packet.payload.begin() + 2, ssid_length, ssid.begin());
    std::copy_n(inbound.packet.payload.begin() + 2 + ssid_length,
                password_length,
                password.begin());

    const bool stored = settings_store_.update_station(ssid.data(), password.data());
    const bool applied = stored && wifi_manager_.apply_station(settings_store_.settings());
    wireless::Packet response{};
    response.type = wireless::MessageType::wifi_result;
    response.flags = wireless::packet_flag_response |
                     (applied ? wireless::packet_flag_none
                              : wireless::packet_flag_error);
    response.sequence = inbound.packet.sequence;
    response.payload_size = 1;
    response.payload[0] = applied ? 1 : 0;
    send_reply(inbound, response);
    if (!stored) {
        protocol_errors_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(kTag, "could not persist station credentials");
    } else if (!applied) {
        ESP_LOGE(kTag, "station credentials stored but Wi-Fi reconfiguration failed");
    } else {
        ESP_LOGI(kTag, "station credentials updated through secure BLE");
    }
}

} // namespace wireless_esp32
