#include "canopen/node.hpp"

#include <algorithm>

namespace canopen {

Node::Node(uint8_t node_id,
           uint16_t& producer_heartbeat_ms,
           uint8_t& error_register,
           uint32_t& high_resolution_time_us,
           ObjectDictionary& dictionary,
           PdoManager& pdo,
           can::ITransport& transport)
    : node_id_(node_id)
    , producer_heartbeat_ms_(producer_heartbeat_ms)
    , error_register_(error_register)
    , high_resolution_time_us_(high_resolution_time_us)
    , dictionary_(dictionary)
    , pdo_(pdo)
    , transport_(transport)
    , sdo_(node_id, dictionary, transport)
{
}

bool Node::start(uint64_t now_us)
{
    if (started_ || node_id_ == 0 || node_id_ > 127 || !dictionary_.frozen()) {
        return false;
    }
    started_ = true;
    state_ = NmtState::initializing;
    const bool boot_sent = publish_heartbeat(NmtState::initializing);
    enter_state(NmtState::pre_operational, now_us, false);
    return boot_sent;
}

bool Node::handle(const can::Frame& frame, uint64_t now_us)
{
    if (!started_ || frame.extended || frame.remote) {
        return false;
    }
    if (frame.id == cob::nmt && !frame.fd && frame.size == 2) {
        handle_nmt(frame, now_us);
        return true;
    }
    if (state_ != NmtState::stopped && sdo_.handle(frame, now_us)) {
        return true;
    }
    if (frame.id == cob::sync && !frame.fd && frame.size <= 1) {
        pdo_.on_sync(now_us, state_);
        return true;
    }
    return pdo_.handle(frame, state_);
}

void Node::process(uint64_t now_us)
{
    if (!started_) {
        return;
    }
    high_resolution_time_us_ = static_cast<uint32_t>(now_us);
    sdo_.process(now_us);
    pdo_.process(now_us, state_);

    if (producer_heartbeat_ms_ == 0) {
        next_heartbeat_us_ = 0;
        return;
    }
    const uint64_t period_us = static_cast<uint64_t>(producer_heartbeat_ms_) * 1000U;
    if (next_heartbeat_us_ == 0) {
        next_heartbeat_us_ = now_us + period_us;
    } else if (now_us >= next_heartbeat_us_) {
        (void)publish_heartbeat(state_);
        do {
            next_heartbeat_us_ += period_us;
        } while (next_heartbeat_us_ <= now_us);
    }
}

void Node::handle_nmt(const can::Frame& frame, uint64_t now_us)
{
    const uint8_t target = frame.data[1];
    if (target != 0 && target != node_id_) {
        return;
    }
    switch (static_cast<NmtCommand>(frame.data[0])) {
    case NmtCommand::start:
        enter_state(NmtState::operational, now_us, true);
        break;
    case NmtCommand::stop:
        enter_state(NmtState::stopped, now_us, true);
        break;
    case NmtCommand::enter_pre_operational:
        enter_state(NmtState::pre_operational, now_us, true);
        break;
    case NmtCommand::reset_node:
    case NmtCommand::reset_communication:
        reset_communication(now_us);
        break;
    default:
        break;
    }
}

void Node::reset_communication(uint64_t now_us)
{
    state_ = NmtState::initializing;
    sdo_.reset();
    pdo_.reset_communication();
    (void)publish_heartbeat(NmtState::initializing);
    enter_state(NmtState::pre_operational, now_us, false);
}

void Node::enter_state(NmtState state, uint64_t now_us, bool announce)
{
    state_ = state;
    const uint64_t period_us = static_cast<uint64_t>(producer_heartbeat_ms_) * 1000U;
    next_heartbeat_us_ = producer_heartbeat_ms_ == 0 ? 0 : now_us + period_us;
    if (announce) {
        (void)publish_heartbeat(state_);
    }
}

bool Node::publish_heartbeat(NmtState state)
{
    can::Frame frame{};
    frame.id = cob::heartbeat + node_id_;
    frame.size = 1;
    frame.data[0] = static_cast<uint8_t>(state);
    if (transport_.send(frame, 10) != can::SendResult::ok) {
        return false;
    }
    ++heartbeat_count_;
    return true;
}

bool Node::send_emergency(uint16_t error_code, std::span<const uint8_t> manufacturer_data)
{
    can::Frame frame{};
    frame.id = cob::emcy + node_id_;
    frame.size = 8;
    frame.data[0] = static_cast<uint8_t>(error_code);
    frame.data[1] = static_cast<uint8_t>(error_code >> 8U);
    frame.data[2] = error_register_;
    const std::size_t count = std::min<std::size_t>(5, manufacturer_data.size());
    std::copy_n(manufacturer_data.begin(), count, frame.data.begin() + 3);
    return transport_.send(frame, 10) == can::SendResult::ok;
}

} // namespace canopen

