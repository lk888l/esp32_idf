#pragma once

#include <cstdint>

namespace canopen {

enum class NmtState : uint8_t {
    initializing = 0x00,
    stopped = 0x04,
    operational = 0x05,
    pre_operational = 0x7F,
};

enum class NmtCommand : uint8_t {
    start = 0x01,
    stop = 0x02,
    enter_pre_operational = 0x80,
    reset_node = 0x81,
    reset_communication = 0x82,
};

namespace cob {
inline constexpr uint16_t nmt = 0x000;
inline constexpr uint16_t sync = 0x080;
inline constexpr uint16_t emcy = 0x080;
inline constexpr uint16_t time = 0x100;
inline constexpr uint16_t tpdo1 = 0x180;
inline constexpr uint16_t rpdo1 = 0x200;
inline constexpr uint16_t tpdo2 = 0x280;
inline constexpr uint16_t rpdo2 = 0x300;
inline constexpr uint16_t tpdo3 = 0x380;
inline constexpr uint16_t rpdo3 = 0x400;
inline constexpr uint16_t tpdo4 = 0x480;
inline constexpr uint16_t rpdo4 = 0x500;
inline constexpr uint16_t tsdo = 0x580;
inline constexpr uint16_t rsdo = 0x600;
inline constexpr uint16_t heartbeat = 0x700;
} // namespace cob

enum class AbortCode : uint32_t {
    none = 0,
    toggle_bit = 0x05030000,
    protocol_timeout = 0x05040000,
    invalid_command = 0x05040001,
    out_of_memory = 0x05040005,
    unsupported_access = 0x06010000,
    write_only = 0x06010001,
    read_only = 0x06010002,
    object_not_found = 0x06020000,
    not_mappable = 0x06040041,
    pdo_length_exceeded = 0x06040042,
    parameter_incompatible = 0x06040043,
    hardware_error = 0x06060000,
    type_mismatch = 0x06070010,
    data_too_long = 0x06070012,
    data_too_short = 0x06070013,
    subindex_not_found = 0x06090011,
    value_range = 0x06090030,
    general_error = 0x08000000,
    data_store = 0x08000020,
    local_control = 0x08000021,
    device_state = 0x08000022,
};

} // namespace canopen

