#pragma once

#include "can/frame.hpp"
#include "canopen_esp32/esp_twai_transport.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace canopen_esp32 {

enum class GatewayOrigin : uint8_t { physical_bus = 0, local_node = 1, wireless_client = 2 };

struct GatewayFrame {
    can::Frame frame{};
    GatewayOrigin origin = GatewayOrigin::physical_bus;
    uint64_t timestamp_us = 0;
};

struct GatewayConfig {
    uint32_t ingress_queue_depth = 32;
    uint32_t monitor_queue_depth = 64;
};

struct GatewayStatistics {
    uint32_t ingress_frames = 0;
    uint32_t ingress_dropped = 0;
    uint32_t forwarded_frames = 0;
    uint32_t monitor_dropped = 0;
};

class EspCanGateway final : public can::ITransport {
public:
    static constexpr std::size_t kMaxIngressDepth = 128;
    static constexpr std::size_t kMaxMonitorDepth = 128;

    EspCanGateway(TwaiConfig twai_config, GatewayConfig gateway_config)
        : physical_(twai_config), config_(gateway_config)
    {
    }

    bool initialize();
    bool start() { return physical_.start(); }
    bool stop() { return physical_.stop(); }
    void maintenance() { physical_.maintenance(); }

    can::SendResult send(const can::Frame& frame, uint32_t timeout_ms) override;
    can::SendResult send_to_bus(const can::Frame& frame, uint32_t timeout_ms);
    bool receive_from_bus(can::Frame& frame, uint32_t timeout_ms)
    {
        return physical_.receive(frame, timeout_ms);
    }

    bool inject(const can::Frame& frame, uint32_t timeout_ms = 0);
    bool receive_injected(can::Frame& frame, uint32_t timeout_ms = 0);
    void observe(const can::Frame& frame, GatewayOrigin origin, uint64_t timestamp_us);
    bool receive_forwarded(GatewayFrame& frame, uint32_t timeout_ms = 0);

    [[nodiscard]] TwaiStatistics twai_statistics() const { return physical_.statistics(); }
    [[nodiscard]] GatewayStatistics gateway_statistics() const;

private:
    EspTwaiTransport physical_;
    GatewayConfig config_;
    StaticQueue_t ingress_queue_control_{};
    std::array<uint8_t, kMaxIngressDepth * sizeof(can::Frame)> ingress_storage_{};
    QueueHandle_t ingress_queue_ = nullptr;
    StaticQueue_t monitor_queue_control_{};
    std::array<uint8_t, kMaxMonitorDepth * sizeof(GatewayFrame)> monitor_storage_{};
    QueueHandle_t monitor_queue_ = nullptr;
    std::atomic<uint32_t> ingress_frames_{0};
    std::atomic<uint32_t> ingress_dropped_{0};
    std::atomic<uint32_t> forwarded_frames_{0};
    std::atomic<uint32_t> monitor_dropped_{0};
};

} // namespace canopen_esp32
