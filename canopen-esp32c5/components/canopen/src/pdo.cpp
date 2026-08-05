#include "canopen/pdo.hpp"

#include <algorithm>
#include <array>

namespace canopen {
namespace {

constexpr std::array<uint16_t, PdoManager::kPdoCount> kRpdoBases{
    cob::rpdo1, cob::rpdo2, cob::rpdo3, cob::rpdo4};
constexpr std::array<uint16_t, PdoManager::kPdoCount> kTpdoBases{
    cob::tpdo1, cob::tpdo2, cob::tpdo3, cob::tpdo4};

uint32_t load_le32(std::span<const uint8_t> source)
{
    return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8U) |
           (static_cast<uint32_t>(source[2]) << 16U) |
           (static_cast<uint32_t>(source[3]) << 24U);
}

bool valid_transmission_type(uint8_t value)
{
    return value <= 240 || value == 254 || value == 255;
}

bool cob_enabled(uint32_t value)
{
    return (value & 0x80000000U) == 0;
}

} // namespace

PdoManager::PdoManager(uint8_t node_id,
                       ObjectDictionary& dictionary,
                       can::ITransport& transport,
                       bool fd_enabled)
    : node_id_(node_id)
    , dictionary_(dictionary)
    , transport_(transport)
    , fd_enabled_(fd_enabled)
{
    for (std::size_t channel = 0; channel < kPdoCount; ++channel) {
        rpdo_[channel].cob_id = kRpdoBases[channel] + node_id_;
        tpdo_[channel].cob_id = 0x40000000U | (kTpdoBases[channel] + node_id_);
    }
    rpdo_defaults_ = rpdo_;
    tpdo_defaults_ = tpdo_;
}

AbortCode PdoManager::register_dictionary_entries()
{
    for (std::size_t channel = 0; channel < kPdoCount; ++channel) {
        const uint16_t rpdo_comm = static_cast<uint16_t>(0x1400U + channel);
        const uint16_t rpdo_map = static_cast<uint16_t>(0x1600U + channel);
        const uint16_t tpdo_comm = static_cast<uint16_t>(0x1800U + channel);
        const uint16_t tpdo_map = static_cast<uint16_t>(0x1A00U + channel);
        auto add = [this](AbortCode result) { return result == AbortCode::none; };

        if (!add(dictionary_.add_scalar({rpdo_comm, 0},
                                        rpdo_[channel].communication_sub_count,
                                        Access::read_only)) ||
            !add(dictionary_.add_scalar({rpdo_comm, 1},
                                        rpdo_[channel].cob_id,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({rpdo_comm, 2},
                                        rpdo_[channel].transmission_type,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({rpdo_map, 0},
                                        rpdo_[channel].mapping_count,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this))) {
            return AbortCode::out_of_memory;
        }
        for (std::size_t mapping = 0; mapping < kMaxMappings; ++mapping) {
            if (!add(dictionary_.add_scalar({rpdo_map, static_cast<uint8_t>(mapping + 1)},
                                            rpdo_[channel].mappings[mapping],
                                            Access::read_write,
                                            false,
                                            config_write_hook,
                                            this))) {
                return AbortCode::out_of_memory;
            }
        }

        if (!add(dictionary_.add_scalar({tpdo_comm, 0},
                                        tpdo_[channel].communication_sub_count,
                                        Access::read_only)) ||
            !add(dictionary_.add_scalar({tpdo_comm, 1},
                                        tpdo_[channel].cob_id,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({tpdo_comm, 2},
                                        tpdo_[channel].transmission_type,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({tpdo_comm, 3},
                                        tpdo_[channel].inhibit_time_100us,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({tpdo_comm, 4},
                                        tpdo_[channel].compatibility_sub4,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({tpdo_comm, 5},
                                        tpdo_[channel].event_timer_ms,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this)) ||
            !add(dictionary_.add_scalar({tpdo_map, 0},
                                        tpdo_[channel].mapping_count,
                                        Access::read_write,
                                        false,
                                        config_write_hook,
                                        this))) {
            return AbortCode::out_of_memory;
        }
        for (std::size_t mapping = 0; mapping < kMaxMappings; ++mapping) {
            if (!add(dictionary_.add_scalar({tpdo_map, static_cast<uint8_t>(mapping + 1)},
                                            tpdo_[channel].mappings[mapping],
                                            Access::read_write,
                                            false,
                                            config_write_hook,
                                            this))) {
                return AbortCode::out_of_memory;
            }
        }
    }
    return AbortCode::none;
}

AbortCode PdoManager::config_write_hook(const Entry& entry,
                                        std::span<const uint8_t> source,
                                        void* context)
{
    return static_cast<PdoManager*>(context)->validate_config_write(entry, source);
}

AbortCode PdoManager::validate_config_write(const Entry& entry,
                                            std::span<const uint8_t> source)
{
    const uint16_t index = entry.key.index;
    bool receive = false;
    bool communication = false;
    std::size_t channel = 0;
    if (index >= 0x1400 && index < 0x1400 + kPdoCount) {
        receive = true;
        communication = true;
        channel = index - 0x1400;
    } else if (index >= 0x1600 && index < 0x1600 + kPdoCount) {
        receive = true;
        channel = index - 0x1600;
    } else if (index >= 0x1800 && index < 0x1800 + kPdoCount) {
        communication = true;
        channel = index - 0x1800;
    } else if (index >= 0x1A00 && index < 0x1A00 + kPdoCount) {
        channel = index - 0x1A00;
    } else {
        return AbortCode::general_error;
    }

    if (communication) {
        if (entry.key.subindex == 1) {
            const uint32_t candidate = load_le32(source);
            if ((candidate & ~0xE00007FFU) != 0 || (candidate & 0x20000000U) != 0) {
                return AbortCode::value_range;
            }
            const uint32_t current = receive ? rpdo_[channel].cob_id : tpdo_[channel].cob_id;
            if (cob_enabled(current) && candidate != current && cob_enabled(candidate)) {
                return AbortCode::device_state;
            }
        } else if (entry.key.subindex == 2 && !valid_transmission_type(source[0])) {
            return AbortCode::value_range;
        } else if (entry.key.subindex == 3 && source.size() != sizeof(uint16_t)) {
            return AbortCode::type_mismatch;
        }
        ++config_revision_;
        return AbortCode::none;
    }

    const uint8_t active_count = receive ? rpdo_[channel].mapping_count
                                         : tpdo_[channel].mapping_count;
    if (entry.key.subindex == 0) {
        const uint8_t count = source[0];
        if (count > kMaxMappings) {
            return AbortCode::value_range;
        }
        const AbortCode result = validate_mapping_set(channel, receive, count);
        if (result != AbortCode::none) {
            return result;
        }
    } else {
        if (active_count != 0) {
            return AbortCode::device_state;
        }
        const AbortCode result = validate_mapping_word(load_le32(source), receive);
        if (result != AbortCode::none) {
            return result;
        }
    }
    ++config_revision_;
    return AbortCode::none;
}

AbortCode PdoManager::validate_mapping_word(uint32_t word, bool receive) const
{
    const OdKey key{static_cast<uint16_t>(word >> 16U), static_cast<uint8_t>(word >> 8U)};
    const uint8_t bits = static_cast<uint8_t>(word);
    if (bits == 0 || (bits % 8U) != 0) {
        return AbortCode::not_mappable;
    }
    const Entry* entry = dictionary_.find(key);
    if (entry == nullptr || !entry->pdo_mappable || entry->storage.size() * 8U != bits) {
        return AbortCode::not_mappable;
    }
    if (receive && !is_writable(entry->access)) {
        return AbortCode::read_only;
    }
    if (!receive && !is_readable(entry->access)) {
        return AbortCode::write_only;
    }
    return AbortCode::none;
}

AbortCode PdoManager::validate_mapping_set(std::size_t channel,
                                           bool receive,
                                           uint8_t count) const
{
    std::size_t bits = 0;
    for (std::size_t mapping = 0; mapping < count; ++mapping) {
        const uint32_t word = receive ? rpdo_[channel].mappings[mapping]
                                      : tpdo_[channel].mappings[mapping];
        const AbortCode result = validate_mapping_word(word, receive);
        if (result != AbortCode::none) {
            return result;
        }
        bits += static_cast<uint8_t>(word);
    }
    const std::size_t maximum = fd_enabled_ ? 512U : 64U;
    return bits <= maximum ? AbortCode::none : AbortCode::pdo_length_exceeded;
}

void PdoManager::refresh_mapping(std::size_t channel, bool receive)
{
    RuntimeMapping& runtime = receive ? rx_runtime_[channel] : tx_runtime_[channel];
    if (runtime.revision == config_revision_) {
        return;
    }
    runtime.valid = false;
    runtime.payload_size = 0;
    runtime.revision = config_revision_;
    const uint8_t count = receive ? rpdo_[channel].mapping_count : tpdo_[channel].mapping_count;
    if (validate_mapping_set(channel, receive, count) != AbortCode::none) {
        return;
    }
    for (std::size_t mapping = 0; mapping < count; ++mapping) {
        const uint32_t word = receive ? rpdo_[channel].mappings[mapping]
                                      : tpdo_[channel].mappings[mapping];
        runtime.keys[mapping] = {
            static_cast<uint16_t>(word >> 16U), static_cast<uint8_t>(word >> 8U)};
        runtime.byte_lengths[mapping] = static_cast<uint8_t>(word) / 8U;
        runtime.payload_size += runtime.byte_lengths[mapping];
    }
    runtime.valid = count > 0;
}

bool PdoManager::handle(const can::Frame& frame, NmtState state)
{
    if (state != NmtState::operational || frame.extended || frame.remote || !frame.valid()) {
        return false;
    }
    for (std::size_t channel = 0; channel < kPdoCount; ++channel) {
        if (!cob_enabled(rpdo_[channel].cob_id) ||
            frame.id != (rpdo_[channel].cob_id & 0x7FFU)) {
            continue;
        }
        refresh_mapping(channel, true);
        RuntimeMapping& runtime = rx_runtime_[channel];
        if (!runtime.valid || frame.size < runtime.payload_size) {
            return true;
        }
        std::size_t offset = 0;
        for (std::size_t mapping = 0; mapping < rpdo_[channel].mapping_count; ++mapping) {
            const std::size_t length = runtime.byte_lengths[mapping];
            if (dictionary_.write(runtime.keys[mapping], {frame.data.data() + offset, length}) !=
                AbortCode::none) {
                return true;
            }
            offset += length;
        }
        ++received_count_;
        return true;
    }
    return false;
}

void PdoManager::on_sync(uint64_t now_us, NmtState state)
{
    if (state != NmtState::operational) {
        return;
    }
    for (std::size_t channel = 0; channel < kPdoCount; ++channel) {
        const uint8_t type = tpdo_[channel].transmission_type;
        if (type >= 1 && type <= 240) {
            RuntimeMapping& runtime = tx_runtime_[channel];
            if (++runtime.sync_counter >= type) {
                runtime.sync_counter = 0;
                (void)transmit(channel, now_us);
            }
        }
    }
}

void PdoManager::process(uint64_t now_us, NmtState state)
{
    if (state != NmtState::operational) {
        return;
    }
    for (std::size_t channel = 0; channel < kPdoCount; ++channel) {
        const auto& config = tpdo_[channel];
        if ((config.transmission_type == 254 || config.transmission_type == 255) &&
            config.event_timer_ms > 0) {
            const uint64_t period_us = static_cast<uint64_t>(config.event_timer_ms) * 1000U;
            if (tx_runtime_[channel].last_transmit_us == 0 ||
                now_us - tx_runtime_[channel].last_transmit_us >= period_us) {
                (void)transmit(channel, now_us);
            }
        }
    }
}

bool PdoManager::transmit(std::size_t channel, uint64_t now_us)
{
    const auto& config = tpdo_[channel];
    if (!cob_enabled(config.cob_id)) {
        return false;
    }
    RuntimeMapping& runtime = tx_runtime_[channel];
    refresh_mapping(channel, false);
    if (!runtime.valid) {
        return false;
    }
    const uint64_t inhibit_us = static_cast<uint64_t>(config.inhibit_time_100us) * 100U;
    if (runtime.last_transmit_us != 0 && now_us - runtime.last_transmit_us < inhibit_us) {
        return false;
    }

    can::Frame frame{};
    frame.id = config.cob_id & 0x7FFU;
    frame.size = static_cast<uint8_t>(runtime.payload_size);
    frame.fd = frame.size > can::kClassicMaxPayload;
    frame.bitrate_switch = frame.fd;
    std::size_t offset = 0;
    for (std::size_t mapping = 0; mapping < config.mapping_count; ++mapping) {
        std::array<uint8_t, 8> value{};
        std::size_t size = 0;
        if (dictionary_.read(runtime.keys[mapping], value, size) != AbortCode::none ||
            size != runtime.byte_lengths[mapping]) {
            return false;
        }
        std::copy_n(value.begin(), size, frame.data.begin() + offset);
        offset += size;
    }
    if (transport_.send(frame, 1) != can::SendResult::ok) {
        return false;
    }
    runtime.last_transmit_us = now_us;
    ++transmitted_count_;
    return true;
}

void PdoManager::reset_communication()
{
    rpdo_ = rpdo_defaults_;
    tpdo_ = tpdo_defaults_;
    ++config_revision_;
    for (auto& runtime : rx_runtime_) {
        runtime = {};
    }
    for (auto& runtime : tx_runtime_) {
        runtime = {};
    }
}

} // namespace canopen
