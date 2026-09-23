#pragma once
#include <cstddef>
#include <cstdint>
#include "ble_gateway.hpp"
#include "esp_err.h"
struct cJSON;
namespace connectivity {
class BleTransport;
namespace gateway {
using Submit = esp_err_t (*)(void*, const Request&, uint32_t id, uint32_t* ticket);
// Called only after the common envelope and origin authentication are checked.
// Queries copy bounded snapshots; mutations enqueue and never perform RF IO.
void dispatch(const cJSON* root, uint32_t id, BleTransport& ble, Submit submit,
              void* context, char* output, size_t capacity);
}
}
