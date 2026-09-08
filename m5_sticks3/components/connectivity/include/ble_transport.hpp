#pragma once

#include <atomic>

#include "ble_diagnostics.hpp"
#include "connectivity_types.hpp"
#include "esp_err.h"

namespace connectivity {

// Owns the single NimBLE host. Lifecycle and command submission belong to the
// module owner task. Snapshots may be called concurrently by application tasks.
class BleTransport final {
public:
    BleTransport();
    ~BleTransport();

    BleTransport(const BleTransport&) = delete;
    BleTransport& operator=(const BleTransport&) = delete;

    esp_err_t start(const char* name, RequestHandler handler, void* context);
    esp_err_t stop();
    BleSnapshot snapshot();

    // Accepted commands execute on the host event queue. Async stack errors
    // are exposed through diagnostics; process never performs blocking RF I/O.
    esp_err_t request_scan();
    esp_err_t connect_peer(const char* address, uint8_t address_type);
    esp_err_t disconnect_peer();
    esp_err_t set_enabled(bool enabled);
    void process();
    BleDiagnostics diagnostics();

private:
    struct Impl;
    std::atomic<Impl*> impl_{nullptr};
};

} // namespace connectivity
