#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "infrared_protocol.hpp"

namespace infrared {
inline constexpr size_t kSlotCount = 4;
enum class State : uint8_t { Idle, Listening, Learning, Transmitting, Error };
struct Snapshot {
    bool initialized = false;
    bool busy = false;
    bool listening = false;
    bool learning = false;
    bool dma_tx = false;
    bool dma_rx = false;
    uint8_t learning_slot = 0;
    uint32_t received_frames = 0;
    uint32_t transmitted_frames = 0;
    uint32_t dropped_frames = 0;
    esp_err_t last_error = ESP_OK;
    protocol::Decoded last_decoded{};
    size_t last_symbol_count = 0;
    std::array<bool, kSlotCount> slot_used{};
    State state = State::Idle;
    uint32_t carrier_hz = protocol::kDefaultCarrierHz;
    uint8_t duty_percent = 33;
};

// Lifecycle belongs to the application task. Commands are bounded non-blocking
// submissions; completion/errors appear in snapshots. stop() bypasses the queue.
class Service {
public:
    static Service& instance();
    esp_err_t initialize();
    esp_err_t deinitialize();
    esp_err_t listen();
    esp_err_t stop();
    esp_err_t learn(uint8_t slot);
    esp_err_t transmit_nec(uint16_t address, uint8_t command, bool extended = false, uint8_t repeats = 0);
    // Copies the envelope before returning; the caller may reuse/release frame.
    // One pending raw payload is allowed; a second returns INVALID_STATE.
    esp_err_t transmit_raw(const protocol::Frame& frame);
    esp_err_t replay_last();
    esp_err_t replay_slot(uint8_t slot);
    esp_err_t save_last(uint8_t slot);
    esp_err_t erase_slot(uint8_t slot);
    esp_err_t set_carrier(uint32_t frequency_hz, uint8_t duty_percent = 33);
    Snapshot snapshot() const;
    esp_err_t copy_last_frame(protocol::Frame& output) const;
private:
    Service() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace infrared
