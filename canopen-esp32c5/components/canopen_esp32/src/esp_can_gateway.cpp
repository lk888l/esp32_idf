#include "canopen_esp32/esp_can_gateway.hpp"

#include "esp_timer.h"

namespace canopen_esp32 {

bool EspCanGateway::initialize()
{
    if (ingress_queue_ != nullptr || monitor_queue_ != nullptr ||
        config_.ingress_queue_depth == 0 ||
        config_.ingress_queue_depth > kMaxIngressDepth ||
        config_.monitor_queue_depth == 0 ||
        config_.monitor_queue_depth > kMaxMonitorDepth) {
        return false;
    }

    ingress_queue_ = xQueueCreateStatic(config_.ingress_queue_depth,
                                        sizeof(can::Frame),
                                        ingress_storage_.data(),
                                        &ingress_queue_control_);
    monitor_queue_ = xQueueCreateStatic(config_.monitor_queue_depth,
                                        sizeof(GatewayFrame),
                                        monitor_storage_.data(),
                                        &monitor_queue_control_);
    return ingress_queue_ != nullptr && monitor_queue_ != nullptr && physical_.initialize();
}

can::SendResult EspCanGateway::send(const can::Frame& frame, uint32_t timeout_ms)
{
    if (!frame.valid()) {
        return can::SendResult::invalid_frame;
    }
    const can::SendResult result = physical_.send(frame, timeout_ms);
    observe(frame, GatewayOrigin::local_node, static_cast<uint64_t>(esp_timer_get_time()));
    return result;
}

can::SendResult EspCanGateway::send_to_bus(const can::Frame& frame, uint32_t timeout_ms)
{
    return physical_.send(frame, timeout_ms);
}

bool EspCanGateway::inject(const can::Frame& frame, uint32_t timeout_ms)
{
    if (!frame.valid() || ingress_queue_ == nullptr) {
        ingress_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(ingress_queue_, &frame, ticks) != pdTRUE) {
        ingress_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ingress_frames_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EspCanGateway::receive_injected(can::Frame& frame, uint32_t timeout_ms)
{
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return ingress_queue_ != nullptr && xQueueReceive(ingress_queue_, &frame, ticks) == pdTRUE;
}

void EspCanGateway::observe(const can::Frame& frame,
                            GatewayOrigin origin,
                            uint64_t timestamp_us)
{
    if (monitor_queue_ == nullptr) {
        monitor_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const GatewayFrame routed{.frame = frame, .origin = origin, .timestamp_us = timestamp_us};
    if (xQueueSend(monitor_queue_, &routed, 0) == pdTRUE) {
        forwarded_frames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        monitor_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool EspCanGateway::receive_forwarded(GatewayFrame& frame, uint32_t timeout_ms)
{
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return monitor_queue_ != nullptr && xQueueReceive(monitor_queue_, &frame, ticks) == pdTRUE;
}

GatewayStatistics EspCanGateway::gateway_statistics() const
{
    return {
        .ingress_frames = ingress_frames_.load(std::memory_order_relaxed),
        .ingress_dropped = ingress_dropped_.load(std::memory_order_relaxed),
        .forwarded_frames = forwarded_frames_.load(std::memory_order_relaxed),
        .monitor_dropped = monitor_dropped_.load(std::memory_order_relaxed),
    };
}

} // namespace canopen_esp32
