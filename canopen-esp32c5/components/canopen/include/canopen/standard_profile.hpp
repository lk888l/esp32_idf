#pragma once

#include "can/frame.hpp"
#include "canopen/node.hpp"
#include "canopen/object_dictionary.hpp"
#include "canopen/parameter_storage.hpp"
#include "canopen/pdo.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace canopen {

struct Identity {
    uint32_t vendor_id = 0;
    uint32_t product_code = 0;
    uint32_t revision_number = 0;
    uint32_t serial_number = 0;
};

struct ProfileConfig {
    uint8_t node_id = 1;
    uint16_t producer_heartbeat_ms = 100;
    uint32_t device_type = 0;
    Identity identity{};
    std::string_view device_name = "CANopen ESP32-C5";
    std::string_view hardware_version = "ESP32-C5";
    std::string_view software_version = "0.2.0";
    bool can_fd_pdo = true;
    ParameterStorage* parameter_storage = nullptr;
};

class StandardProfile {
public:
    StandardProfile(const ProfileConfig& config, can::ITransport& transport);

    AbortCode initialize();
    bool start(uint64_t now_us);
    bool handle(const can::Frame& frame, uint64_t now_us);
    void process(uint64_t now_us);

    [[nodiscard]] ObjectDictionary& dictionary() { return dictionary_; }
    [[nodiscard]] Node& node() { return node_; }
    [[nodiscard]] const Node& node() const { return node_; }
    [[nodiscard]] uint32_t& application_value() { return application_value_; }
    [[nodiscard]] uint8_t configured_node_id() const { return configured_node_id_; }

private:
    static constexpr std::size_t kStringCapacity = 64;
    static constexpr std::size_t kHeartbeatConsumers = 4;

    static std::size_t copy_string(std::string_view source,
                                   std::array<uint8_t, kStringCapacity>& destination);
    static AbortCode read_store_capabilities(const Entry& entry,
                                             std::span<uint8_t> destination,
                                             void* context);
    static AbortCode write_store_parameters(const Entry& entry,
                                            std::span<const uint8_t> source,
                                            void* context);
    static AbortCode write_configured_node_id(const Entry& entry,
                                              std::span<const uint8_t> source,
                                              void* context);
    AbortCode register_standard_objects();

    ProfileConfig config_;
    ObjectDictionary dictionary_{};
    uint32_t device_type_ = 0;
    uint8_t error_register_ = 0;
    uint32_t sync_cob_id_ = cob::sync;
    uint32_t communication_cycle_period_us_ = 0;
    uint32_t high_resolution_time_us_ = 0;
    uint8_t heartbeat_consumer_count_ = kHeartbeatConsumers;
    std::array<uint32_t, kHeartbeatConsumers> heartbeat_consumers_{};
    uint16_t producer_heartbeat_ms_ = 100;
    uint8_t store_parameter_count_ = 1;
    uint32_t store_all_parameters_ = 0;
    uint8_t identity_count_ = 4;
    Identity identity_{};
    uint8_t sdo_parameter_count_ = 2;
    uint32_t sdo_receive_cob_id_ = 0;
    uint32_t sdo_transmit_cob_id_ = 0;
    std::array<uint8_t, kStringCapacity> device_name_{};
    std::array<uint8_t, kStringCapacity> hardware_version_{};
    std::array<uint8_t, kStringCapacity> software_version_{};
    std::size_t device_name_size_ = 0;
    std::size_t hardware_version_size_ = 0;
    std::size_t software_version_size_ = 0;
    uint8_t manufacturer_object_count_ = 4;
    uint32_t uptime_ms_ = 0;
    uint32_t received_pdo_count_ = 0;
    uint32_t transmitted_pdo_count_ = 0;
    uint32_t application_value_ = 0;
    uint8_t node_configuration_count_ = 1;
    uint8_t configured_node_id_ = 1;
    PdoManager pdo_;
    Node node_;
    bool initialized_ = false;
    uint64_t start_time_us_ = 0;
};

} // namespace canopen
