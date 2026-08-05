#pragma once

#include "can/frame.hpp"
#include "canopen/object_dictionary.hpp"
#include "canopen/pdo.hpp"
#include "canopen/sdo_server.hpp"
#include "canopen/types.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace canopen {

class Node {
public:
    Node(uint8_t node_id,
         uint16_t& producer_heartbeat_ms,
         uint8_t& error_register,
         uint32_t& high_resolution_time_us,
         ObjectDictionary& dictionary,
         PdoManager& pdo,
         can::ITransport& transport);

    bool start(uint64_t now_us);
    bool handle(const can::Frame& frame, uint64_t now_us);
    void process(uint64_t now_us);
    bool send_emergency(uint16_t error_code, std::span<const uint8_t> manufacturer_data = {});

    [[nodiscard]] NmtState state() const { return state_; }
    [[nodiscard]] uint8_t node_id() const { return node_id_; }
    [[nodiscard]] uint32_t heartbeat_count() const { return heartbeat_count_; }

private:
    void handle_nmt(const can::Frame& frame, uint64_t now_us);
    bool publish_heartbeat(NmtState state);
    void enter_state(NmtState state, uint64_t now_us, bool announce);
    void reset_communication(uint64_t now_us);

    uint8_t node_id_;
    uint16_t& producer_heartbeat_ms_;
    uint8_t& error_register_;
    uint32_t& high_resolution_time_us_;
    ObjectDictionary& dictionary_;
    PdoManager& pdo_;
    can::ITransport& transport_;
    SdoServer sdo_;
    NmtState state_ = NmtState::initializing;
    uint64_t next_heartbeat_us_ = 0;
    uint32_t heartbeat_count_ = 0;
    bool started_ = false;
};

} // namespace canopen

