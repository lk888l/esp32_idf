#pragma once

#include "canopen/parameter_storage.hpp"

#include "nvs.h"

#include <cstdint>

namespace canopen_esp32 {

class EspNvsParameterStorage final : public canopen::ParameterStorage {
public:
    explicit EspNvsParameterStorage(uint8_t default_node_id);
    ~EspNvsParameterStorage() override;

    EspNvsParameterStorage(const EspNvsParameterStorage&) = delete;
    EspNvsParameterStorage& operator=(const EspNvsParameterStorage&) = delete;

    bool store_node_id(uint8_t node_id) override;

    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] bool restored() const { return restored_; }
    [[nodiscard]] uint8_t startup_node_id() const { return startup_node_id_; }

private:
    static constexpr const char* kNamespace = "canopen";
    static constexpr const char* kNodeIdKey = "node_id";

    nvs_handle_t handle_ = 0;
    uint8_t startup_node_id_ = 1;
    uint8_t stored_node_id_ = 0;
    bool ready_ = false;
    bool restored_ = false;
};

} // namespace canopen_esp32
