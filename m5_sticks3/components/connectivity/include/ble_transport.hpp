#pragma once

#include <atomic>

#include "connectivity_types.hpp"
#include "esp_err.h"

namespace connectivity {

// Owns the single NimBLE host. start()/stop() belong to the module lifecycle
// task; snapshot() may be called concurrently by any application task.
class BleTransport final {
public:
    BleTransport();
    ~BleTransport();

    BleTransport(const BleTransport&) = delete;
    BleTransport& operator=(const BleTransport&) = delete;

    esp_err_t start(const char* name, RequestHandler handler, void* context);
    esp_err_t stop();
    BleSnapshot snapshot();

private:
    struct Impl;
    std::atomic<Impl*> impl_{nullptr};
};

} // namespace connectivity
