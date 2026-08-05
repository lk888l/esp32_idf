#include "canopen/standard_profile.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

namespace canopen {

StandardProfile::StandardProfile(const ProfileConfig& config, can::ITransport& transport)
    : config_(config)
    , device_type_(config.device_type)
    , producer_heartbeat_ms_(config.producer_heartbeat_ms)
    , store_all_parameters_(config.parameter_storage == nullptr ? 0U : 1U)
    , identity_(config.identity)
    , sdo_receive_cob_id_(cob::rsdo + config.node_id)
    , sdo_transmit_cob_id_(cob::tsdo + config.node_id)
    , device_name_size_(copy_string(config.device_name, device_name_))
    , hardware_version_size_(copy_string(config.hardware_version, hardware_version_))
    , software_version_size_(copy_string(config.software_version, software_version_))
    , configured_node_id_(config.node_id)
    , pdo_(config.node_id, dictionary_, transport, config.can_fd_pdo)
    , node_(config.node_id,
            producer_heartbeat_ms_,
            error_register_,
            high_resolution_time_us_,
            dictionary_,
            pdo_,
            transport)
{
}

std::size_t StandardProfile::copy_string(
    std::string_view source,
    std::array<uint8_t, StandardProfile::kStringCapacity>& destination)
{
    const std::size_t count = std::min(source.size(), destination.size());
    std::copy_n(reinterpret_cast<const uint8_t*>(source.data()), count, destination.begin());
    return count;
}

AbortCode StandardProfile::read_store_capabilities(const Entry&,
                                                    std::span<uint8_t> destination,
                                                    void* context)
{
    const auto* self = static_cast<const StandardProfile*>(context);
    const uint32_t capabilities = self->config_.parameter_storage == nullptr ? 0U : 1U;
    destination[0] = static_cast<uint8_t>(capabilities);
    destination[1] = static_cast<uint8_t>(capabilities >> 8U);
    destination[2] = static_cast<uint8_t>(capabilities >> 16U);
    destination[3] = static_cast<uint8_t>(capabilities >> 24U);
    return AbortCode::none;
}

AbortCode StandardProfile::write_store_parameters(const Entry&,
                                                   std::span<const uint8_t> source,
                                                   void* context)
{
    constexpr uint32_t kSaveSignature = 0x65766173U;
    const uint32_t signature = static_cast<uint32_t>(source[0]) |
                               (static_cast<uint32_t>(source[1]) << 8U) |
                               (static_cast<uint32_t>(source[2]) << 16U) |
                               (static_cast<uint32_t>(source[3]) << 24U);
    if (signature != kSaveSignature) {
        return AbortCode::data_store;
    }

    auto* self = static_cast<StandardProfile*>(context);
    if (self->config_.parameter_storage == nullptr ||
        !self->config_.parameter_storage->store_node_id(self->configured_node_id_)) {
        return AbortCode::data_store;
    }
    return AbortCode::none;
}

AbortCode StandardProfile::write_configured_node_id(const Entry&,
                                                     std::span<const uint8_t> source,
                                                     void*)
{
    return source[0] >= 1 && source[0] <= 127 ? AbortCode::none : AbortCode::value_range;
}

AbortCode StandardProfile::initialize()
{
    if (initialized_) {
        return AbortCode::none;
    }
    if (config_.node_id == 0 || config_.node_id > 127) {
        return AbortCode::value_range;
    }
    AbortCode result = register_standard_objects();
    if (result != AbortCode::none) {
        return result;
    }
    result = pdo_.register_dictionary_entries();
    if (result != AbortCode::none) {
        return result;
    }
    dictionary_.freeze();
    initialized_ = true;
    return AbortCode::none;
}

AbortCode StandardProfile::register_standard_objects()
{
    auto require = [](AbortCode result) { return result == AbortCode::none; };
    if (!require(dictionary_.add_scalar({0x1000, 0}, device_type_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1001, 0}, error_register_, Access::read_only, true)) ||
        !require(dictionary_.add_scalar({0x1005, 0}, sync_cob_id_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1006, 0}, communication_cycle_period_us_, Access::read_write)) ||
        !require(dictionary_.add_bytes({0x1008, 0},
                                       DataType::visible_string,
                                       {device_name_.data(), device_name_size_},
                                       Access::read_only)) ||
        !require(dictionary_.add_bytes({0x1009, 0},
                                       DataType::visible_string,
                                       {hardware_version_.data(), hardware_version_size_},
                                       Access::read_only)) ||
        !require(dictionary_.add_bytes({0x100A, 0},
                                       DataType::visible_string,
                                       {software_version_.data(), software_version_size_},
                                       Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1013, 0}, high_resolution_time_us_, Access::read_only, true)) ||
        !require(dictionary_.add_scalar(
            {0x1016, 0}, heartbeat_consumer_count_, Access::read_only))) {
        return AbortCode::out_of_memory;
    }
    for (std::size_t index = 0; index < heartbeat_consumers_.size(); ++index) {
        if (!require(dictionary_.add_scalar({0x1016, static_cast<uint8_t>(index + 1)},
                                            heartbeat_consumers_[index],
                                            Access::read_write))) {
            return AbortCode::out_of_memory;
        }
    }
    if (!require(dictionary_.add_scalar(
            {0x1017, 0}, producer_heartbeat_ms_, Access::read_write)) ||
        !require(dictionary_.add_scalar(
            {0x1010, 0}, store_parameter_count_, Access::read_only)) ||
        !require(dictionary_.add_scalar({0x1010, 1},
                                        store_all_parameters_,
                                        Access::read_write,
                                        false,
                                        write_store_parameters,
                                        this,
                                        read_store_capabilities)) ||
        !require(dictionary_.add_scalar({0x1018, 0}, identity_count_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1018, 1}, identity_.vendor_id, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1018, 2}, identity_.product_code, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1018, 3}, identity_.revision_number, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1018, 4}, identity_.serial_number, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1200, 0}, sdo_parameter_count_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1200, 1}, sdo_receive_cob_id_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x1200, 2}, sdo_transmit_cob_id_, Access::read_only)) ||
        !require(dictionary_.add_scalar(
            {0x2000, 0}, manufacturer_object_count_, Access::read_only)) ||
        !require(dictionary_.add_scalar({0x2000, 1}, uptime_ms_, Access::read_only, true)) ||
        !require(dictionary_.add_scalar(
            {0x2000, 2}, received_pdo_count_, Access::read_only, true)) ||
        !require(dictionary_.add_scalar(
            {0x2000, 3}, transmitted_pdo_count_, Access::read_only, true)) ||
        !require(dictionary_.add_scalar(
            {0x2000, 4}, application_value_, Access::read_write, true)) ||
        !require(dictionary_.add_scalar(
            {0x2001, 0}, node_configuration_count_, Access::read_only)) ||
        !require(dictionary_.add_scalar({0x2001, 1},
                                        configured_node_id_,
                                        Access::read_write,
                                        false,
                                        write_configured_node_id,
                                        this))) {
        return AbortCode::out_of_memory;
    }
    return AbortCode::none;
}

bool StandardProfile::start(uint64_t now_us)
{
    if (!initialized_) {
        return false;
    }
    start_time_us_ = now_us;
    return node_.start(now_us);
}

bool StandardProfile::handle(const can::Frame& frame, uint64_t now_us)
{
    return node_.handle(frame, now_us);
}

void StandardProfile::process(uint64_t now_us)
{
    uptime_ms_ = static_cast<uint32_t>((now_us - start_time_us_) / 1000U);
    received_pdo_count_ = pdo_.received_count();
    transmitted_pdo_count_ = pdo_.transmitted_count();
    node_.process(now_us);
}

} // namespace canopen
