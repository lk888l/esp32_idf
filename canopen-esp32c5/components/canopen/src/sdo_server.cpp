#include "canopen/sdo_server.hpp"

#include "canopen/types.hpp"

#include <algorithm>
#include <cstring>

namespace canopen {
namespace {

uint32_t load_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

void store_le32(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8U);
    data[2] = static_cast<uint8_t>(value >> 16U);
    data[3] = static_cast<uint8_t>(value >> 24U);
}

OdKey request_key(const can::Frame& request)
{
    return {
        static_cast<uint16_t>(request.data[1] | (static_cast<uint16_t>(request.data[2]) << 8U)),
        request.data[3],
    };
}

} // namespace

bool SdoServer::handle(const can::Frame& request, uint64_t now_us)
{
    const uint32_t receive_cob_id = static_cast<uint32_t>(cob::rsdo) + node_id_;
    if (request.extended || request.remote || request.fd || request.id != receive_cob_id ||
        request.size != 8) {
        return false;
    }

    const uint8_t command = request.data[0];
    const uint8_t client_command = command >> 5U;
    if (client_command == 4) {
        reset();
        return true;
    }

    switch (state_) {
    case State::idle:
        handle_initiate(request, now_us);
        break;
    case State::segmented_upload:
        if (client_command == 3) {
            handle_upload_segment(request, now_us);
        } else {
            send_abort(active_key_, AbortCode::invalid_command);
            reset();
        }
        break;
    case State::segmented_download:
        if (client_command == 0) {
            handle_download_segment(request, now_us);
        } else {
            send_abort(active_key_, AbortCode::invalid_command);
            reset();
        }
        break;
    }
    return true;
}

void SdoServer::handle_initiate(const can::Frame& request, uint64_t now_us)
{
    const uint8_t command = request.data[0];
    const uint8_t client_command = command >> 5U;
    const OdKey key = request_key(request);

    if (client_command == 2) {
        std::size_t size = 0;
        const AbortCode result = dictionary_.read(key, transfer_, size);
        if (result != AbortCode::none) {
            send_abort(key, result);
            return;
        }

        std::array<uint8_t, 8> response{};
        response[1] = request.data[1];
        response[2] = request.data[2];
        response[3] = request.data[3];
        if (size <= 4) {
            response[0] = static_cast<uint8_t>(0x43U | ((4U - size) << 2U));
            std::copy_n(transfer_.begin(), size, response.begin() + 4);
            send_response(response);
            return;
        }

        response[0] = 0x41;
        store_le32(response.data() + 4, static_cast<uint32_t>(size));
        active_key_ = key;
        transfer_size_ = size;
        offset_ = 0;
        toggle_ = false;
        deadline_us_ = now_us + kTransferTimeoutUs;
        state_ = State::segmented_upload;
        send_response(response);
        return;
    }

    if (client_command == 1) {
        const bool expedited = (command & 0x02U) != 0;
        const bool size_indicated = (command & 0x01U) != 0;
        if (expedited) {
            const std::size_t unused = size_indicated ? ((command >> 2U) & 0x03U) : 0;
            const std::size_t size = 4U - unused;
            const AbortCode result = dictionary_.write(key, {request.data.data() + 4, size});
            if (result != AbortCode::none) {
                send_abort(key, result);
                return;
            }
            std::array<uint8_t, 8> response{};
            response[0] = 0x60;
            response[1] = request.data[1];
            response[2] = request.data[2];
            response[3] = request.data[3];
            send_response(response);
            return;
        }

        const Entry* entry = dictionary_.find(key);
        if (entry == nullptr) {
            send_abort(key, AbortCode::object_not_found);
            return;
        }
        const std::size_t size = size_indicated ? load_le32(request.data.data() + 4)
                                                : entry->storage.size();
        if (size > transfer_.size()) {
            send_abort(key, AbortCode::out_of_memory);
            return;
        }
        if (size != entry->storage.size()) {
            send_abort(key, size > entry->storage.size() ? AbortCode::data_too_long
                                                         : AbortCode::data_too_short);
            return;
        }

        active_key_ = key;
        transfer_size_ = size;
        offset_ = 0;
        toggle_ = false;
        deadline_us_ = now_us + kTransferTimeoutUs;
        state_ = State::segmented_download;
        std::array<uint8_t, 8> response{};
        response[0] = 0x60;
        response[1] = request.data[1];
        response[2] = request.data[2];
        response[3] = request.data[3];
        send_response(response);
        return;
    }

    send_abort(key, AbortCode::invalid_command);
}

void SdoServer::handle_upload_segment(const can::Frame& request, uint64_t now_us)
{
    const bool request_toggle = (request.data[0] & 0x10U) != 0;
    if (request_toggle != toggle_) {
        send_abort(active_key_, AbortCode::toggle_bit);
        reset();
        return;
    }

    const std::size_t remaining = transfer_size_ - offset_;
    const std::size_t count = std::min<std::size_t>(7, remaining);
    const bool complete = count == remaining;
    const uint8_t unused = static_cast<uint8_t>(7U - count);
    std::array<uint8_t, 8> response{};
    response[0] = static_cast<uint8_t>((toggle_ ? 0x10U : 0U) | (unused << 1U) |
                                       (complete ? 1U : 0U));
    std::copy_n(transfer_.begin() + offset_, count, response.begin() + 1);
    send_response(response);
    offset_ += count;
    if (complete) {
        reset();
    } else {
        toggle_ = !toggle_;
        deadline_us_ = now_us + kTransferTimeoutUs;
    }
}

void SdoServer::handle_download_segment(const can::Frame& request, uint64_t now_us)
{
    const uint8_t command = request.data[0];
    const bool request_toggle = (command & 0x10U) != 0;
    if (request_toggle != toggle_) {
        send_abort(active_key_, AbortCode::toggle_bit);
        reset();
        return;
    }

    const bool complete = (command & 0x01U) != 0;
    const std::size_t unused = (command >> 1U) & 0x07U;
    const std::size_t count = complete ? 7U - unused : 7U;
    if (offset_ + count > transfer_size_) {
        send_abort(active_key_, AbortCode::data_too_long);
        reset();
        return;
    }
    std::copy_n(request.data.begin() + 1, count, transfer_.begin() + offset_);
    offset_ += count;

    if (complete) {
        if (offset_ != transfer_size_) {
            send_abort(active_key_, AbortCode::data_too_short);
            reset();
            return;
        }
        const AbortCode result = dictionary_.write(active_key_, {transfer_.data(), transfer_size_});
        if (result != AbortCode::none) {
            send_abort(active_key_, result);
            reset();
            return;
        }
    }

    std::array<uint8_t, 8> response{};
    response[0] = static_cast<uint8_t>(0x20U | (toggle_ ? 0x10U : 0U));
    send_response(response);
    if (complete) {
        reset();
    } else {
        toggle_ = !toggle_;
        deadline_us_ = now_us + kTransferTimeoutUs;
    }
}

void SdoServer::process(uint64_t now_us)
{
    if (state_ != State::idle && now_us >= deadline_us_) {
        send_abort(active_key_, AbortCode::protocol_timeout);
        reset();
    }
}

void SdoServer::reset()
{
    state_ = State::idle;
    active_key_ = {};
    transfer_size_ = 0;
    offset_ = 0;
    toggle_ = false;
    deadline_us_ = 0;
}

void SdoServer::send_abort(OdKey key, AbortCode code)
{
    std::array<uint8_t, 8> response{};
    response[0] = 0x80;
    response[1] = static_cast<uint8_t>(key.index);
    response[2] = static_cast<uint8_t>(key.index >> 8U);
    response[3] = key.subindex;
    store_le32(response.data() + 4, static_cast<uint32_t>(code));
    send_response(response);
}

void SdoServer::send_response(const std::array<uint8_t, 8>& payload)
{
    can::Frame frame{};
    frame.id = cob::tsdo + node_id_;
    frame.size = 8;
    std::copy(payload.begin(), payload.end(), frame.data.begin());
    (void)transport_.send(frame, 10);
}

} // namespace canopen
