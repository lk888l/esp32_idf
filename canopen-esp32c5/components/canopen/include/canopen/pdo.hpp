#pragma once

#include "can/frame.hpp"
#include "canopen/object_dictionary.hpp"
#include "canopen/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace canopen {

class PdoManager {
public:
    static constexpr std::size_t kPdoCount = 4;
    static constexpr std::size_t kMaxMappings = 16;

    PdoManager(uint8_t node_id,
               ObjectDictionary& dictionary,
               can::ITransport& transport,
               bool fd_enabled = true);

    AbortCode register_dictionary_entries();
    void reset_communication();
    bool handle(const can::Frame& frame, NmtState state);
    void on_sync(uint64_t now_us, NmtState state);
    void process(uint64_t now_us, NmtState state);

    [[nodiscard]] uint32_t received_count() const { return received_count_; }
    [[nodiscard]] uint32_t transmitted_count() const { return transmitted_count_; }

private:
    struct RpdoParameters {
        uint8_t communication_sub_count = 2;
        uint32_t cob_id = 0;
        uint8_t transmission_type = 255;
        uint8_t mapping_count = 0;
        std::array<uint32_t, kMaxMappings> mappings{};
    };

    struct TpdoParameters {
        uint8_t communication_sub_count = 5;
        uint32_t cob_id = 0;
        uint8_t transmission_type = 255;
        uint16_t inhibit_time_100us = 0;
        uint8_t compatibility_sub4 = 0;
        uint16_t event_timer_ms = 0;
        uint8_t mapping_count = 0;
        std::array<uint32_t, kMaxMappings> mappings{};
    };

    struct RuntimeMapping {
        bool valid = false;
        uint16_t payload_size = 0;
        uint16_t sync_counter = 0;
        uint64_t last_transmit_us = 0;
        uint32_t revision = 0;
        std::array<OdKey, kMaxMappings> keys{};
        std::array<uint8_t, kMaxMappings> byte_lengths{};
    };

    static AbortCode config_write_hook(const Entry& entry,
                                       std::span<const uint8_t> source,
                                       void* context);
    AbortCode validate_config_write(const Entry& entry, std::span<const uint8_t> source);
    AbortCode validate_mapping_word(uint32_t word, bool receive) const;
    AbortCode validate_mapping_set(std::size_t channel, bool receive, uint8_t count) const;
    void refresh_mapping(std::size_t channel, bool receive);
    bool transmit(std::size_t channel, uint64_t now_us);

    uint8_t node_id_;
    ObjectDictionary& dictionary_;
    can::ITransport& transport_;
    bool fd_enabled_;
    std::array<RpdoParameters, kPdoCount> rpdo_{};
    std::array<TpdoParameters, kPdoCount> tpdo_{};
    std::array<RpdoParameters, kPdoCount> rpdo_defaults_{};
    std::array<TpdoParameters, kPdoCount> tpdo_defaults_{};
    std::array<RuntimeMapping, kPdoCount> rx_runtime_{};
    std::array<RuntimeMapping, kPdoCount> tx_runtime_{};
    uint32_t config_revision_ = 1;
    uint32_t received_count_ = 0;
    uint32_t transmitted_count_ = 0;
};

} // namespace canopen

