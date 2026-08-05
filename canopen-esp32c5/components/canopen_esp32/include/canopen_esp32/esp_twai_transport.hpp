#pragma once

#include "can/frame.hpp"

#include "driver/gpio.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace canopen_esp32 {

struct TwaiConfig {
    gpio_num_t tx_pin = GPIO_NUM_4;
    gpio_num_t rx_pin = GPIO_NUM_5;
    uint32_t tx_queue_depth = 16;
    uint32_t rx_queue_depth = 32;
};

struct TwaiStatistics {
    uint32_t rx_frames = 0;
    uint32_t rx_dropped = 0;
    uint32_t tx_frames = 0;
    uint32_t tx_failed = 0;
    uint32_t bus_errors = 0;
    uint32_t recoveries = 0;
};

class EspTwaiTransport final : public can::ITransport {
public:
    static constexpr std::size_t kMaxTxSlots = 64;
    static constexpr std::size_t kMaxRxDepth = 128;

    explicit EspTwaiTransport(TwaiConfig config) : config_(config) {}
    ~EspTwaiTransport() override;

    bool initialize();
    bool start();
    bool stop();
    can::SendResult send(const can::Frame& frame, uint32_t timeout_ms) override;
    bool receive(can::Frame& frame, uint32_t timeout_ms);
    void maintenance();
    [[nodiscard]] TwaiStatistics statistics() const;

private:
    struct TxSlot {
        twai_frame_t native{};
        std::array<uint8_t, can::kFdMaxPayload> payload{};
    };

    static bool on_tx_done(twai_node_handle_t,
                           const twai_tx_done_event_data_t* event,
                           void* context);
    static bool on_rx_done(twai_node_handle_t node,
                           const twai_rx_done_event_data_t*,
                           void* context);
    static bool on_state_change(twai_node_handle_t,
                                const twai_state_change_event_data_t* event,
                                void* context);
    static bool on_error(twai_node_handle_t,
                         const twai_error_event_data_t*,
                         void* context);
    static std::size_t canonical_fd_length(std::size_t size);
    int slot_index(const twai_frame_t* frame) const;

    TwaiConfig config_;
    twai_node_handle_t node_ = nullptr;
    std::array<TxSlot, kMaxTxSlots> tx_slots_{};
    StaticQueue_t free_slots_queue_control_{};
    std::array<uint8_t, kMaxTxSlots * sizeof(uint8_t)> free_slots_storage_{};
    QueueHandle_t free_slots_queue_ = nullptr;
    StaticQueue_t rx_queue_control_{};
    std::array<uint8_t, kMaxRxDepth * sizeof(can::Frame)> rx_storage_{};
    QueueHandle_t rx_queue_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> recovery_requested_{false};
    std::atomic<uint32_t> rx_frames_{0};
    std::atomic<uint32_t> rx_dropped_{0};
    std::atomic<uint32_t> tx_frames_{0};
    std::atomic<uint32_t> tx_failed_{0};
    std::atomic<uint32_t> bus_errors_{0};
    std::atomic<uint32_t> recoveries_{0};
};

} // namespace canopen_esp32
