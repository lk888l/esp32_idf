#pragma once
#include "dap_protocol.hpp"
#include "esp_err.h"

namespace debug_probe {
enum class Mode : uint8_t { off, usb, wifi, ble };
struct Snapshot {
    Mode mode = Mode::off;
    bool ready = false, connected = false, swd = false, running = false;
    uint32_t clock_hz = 1000000, limit_hz = 1000000;
    uint32_t packets = 0, rx_bytes = 0, tx_bytes = 0, errors = 0;
    uint32_t last_us = 0;
    uint8_t last_command = 0, last_ack = 0;
    esp_err_t error = ESP_OK;
};
// initialize creates only an idle worker; pins and transports remain off.
esp_err_t initialize();
esp_err_t deinitialize();
void select_mode(Mode mode); // Nonblocking, latest selection wins; off cancels IO.
void set_clock_limit(uint32_t hz);
Snapshot snapshot();
void request_transfer_abort(Mode source); // ISR-free transport callback, no response.
const char* mode_name(Mode mode);

// Existing NimBLE host calls these only for its encrypted peripheral link.
// ATT handlers never wait for SWD; one outstanding command has a retained reply.
bool ble_write(uint16_t connection, const uint8_t* data, size_t size);
size_t ble_read(uint16_t connection, uint8_t* output, size_t capacity);
void ble_disconnect(uint16_t connection);
} // namespace debug_probe
