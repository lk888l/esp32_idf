#pragma once

#include "can/frame.hpp"
#include "canopen/object_dictionary.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace canopen {

class SdoServer {
public:
    static constexpr std::size_t kTransferCapacity = 512;
    static constexpr uint64_t kTransferTimeoutUs = 1'000'000;

    SdoServer(uint8_t node_id, ObjectDictionary& dictionary, can::ITransport& transport)
        : node_id_(node_id), dictionary_(dictionary), transport_(transport)
    {
    }

    bool handle(const can::Frame& request, uint64_t now_us);
    void process(uint64_t now_us);
    void reset();

private:
    enum class State : uint8_t { idle, segmented_upload, segmented_download };

    void handle_initiate(const can::Frame& request, uint64_t now_us);
    void handle_upload_segment(const can::Frame& request, uint64_t now_us);
    void handle_download_segment(const can::Frame& request, uint64_t now_us);
    void send_abort(OdKey key, AbortCode code);
    void send_response(const std::array<uint8_t, 8>& payload);

    uint8_t node_id_;
    ObjectDictionary& dictionary_;
    can::ITransport& transport_;
    State state_ = State::idle;
    OdKey active_key_{};
    std::array<uint8_t, kTransferCapacity> transfer_{};
    std::size_t transfer_size_ = 0;
    std::size_t offset_ = 0;
    bool toggle_ = false;
    uint64_t deadline_us_ = 0;
};

} // namespace canopen
