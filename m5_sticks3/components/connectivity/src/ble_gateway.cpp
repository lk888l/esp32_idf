#include "ble_gateway.hpp"

#include <algorithm>
#include <cstring>

namespace connectivity::gateway {
bool valid(const Request& r)
{
    if (!r.generation || r.length > kWriteBytes) return false;
    switch (r.operation) {
    case Operation::services: case Operation::mtu: case Operation::pair: return true;
    case Operation::characteristics: return r.handle && r.end >= r.handle;
    case Operation::descriptors: return r.handle && r.end > r.handle;
    case Operation::read: case Operation::write: return r.handle != 0;
    case Operation::subscribe: return r.handle && r.mode <= 2;
    }
    return false;
}
bool decode_hex(std::string_view text, uint8_t* output, size_t capacity, uint8_t& length)
{
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    length = 0;
    if (text.size() % 2 || text.size() / 2 > capacity || text.size() / 2 > UINT8_MAX) return false;
    for (const char c : text) if (digit(c) < 0) return false;
    for (size_t i = 0; i < text.size(); i += 2) output[length++] = (digit(text[i]) << 4) | digit(text[i + 1]);
    return true;
}
void encode_hex(const uint8_t* input, size_t length, char* output)
{
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) { output[i * 2] = digits[input[i] >> 4]; output[i * 2 + 1] = digits[input[i] & 15]; }
    output[length * 2] = 0;
}
const char* operation_name(Operation operation)
{
    switch (operation) {
    case Operation::services: return "services";
    case Operation::characteristics: return "characteristics";
    case Operation::descriptors: return "descriptors";
    case Operation::read: return "read";
    case Operation::write: return "write";
    case Operation::subscribe: return "subscribe";
    case Operation::mtu: return "mtu";
    case Operation::pair: return "pair";
    }
    return "unknown";
}
const char* error_name(int32_t error)
{
    switch (error) {
    case none: return "ok";
    case disconnected: return "disconnected";
    case stale_generation: return "stale_generation";
    case busy: return "busy";
    case invalid_request: return "invalid_request";
    case timeout: return "timeout";
    case too_large: return "too_large";
    case transport_busy: return "transport_busy";
    default: return "nimble";
    }
}
void Store::connect()
{
    disconnect();
    if (++status_.generation == 0) ++status_.generation;
    status_.connected = true;
    status_.latest_event = status_.dropped_events = 0;
    events_ = {}; next_event_ = 0;
}
void Store::disconnect(int32_t error)
{
    finish(status_.active_ticket, error);
    status_.connected = false;
    events_ = {}; next_event_ = 0;
}
Store::Result* Store::current()
{
    if (status_.active_ticket) for (auto& result : results_)
        if (result.info.ticket == status_.active_ticket) return &result;
    return nullptr;
}
int32_t Store::reserve(const Request& request, uint32_t ticket)
{
    if (!ticket || !valid(request)) return invalid_request;
    if (!status_.connected) return disconnected;
    if (request.generation != status_.generation) return stale_generation;
    if (status_.active_ticket) return busy;
    auto& result = results_[next_result_++ % kResults];
    result = {};
    result.info.ticket = ticket; result.info.generation = request.generation;
    result.info.operation = request.operation; result.info.handle = request.handle;
    status_.active_ticket = ticket;
    return none;
}
bool Store::active(uint32_t ticket) const
{
    return ticket && status_.connected && status_.active_ticket == ticket;
}
bool Store::start(uint32_t ticket, int64_t now)
{
    if (!active(ticket)) return false;
    current()->info.state = State::running;
    deadline_ = now + kTimeoutUs;
    return true;
}
void Store::finish(uint32_t ticket, int32_t error)
{
    if (!ticket || status_.active_ticket != ticket) return;
    if (auto* result = current()) { result->info.state = State::complete; result->info.error = error; }
    status_.active_ticket = 0; deadline_ = 0;
}
bool Store::expire(int64_t now)
{
    if (!deadline_ || now < deadline_) return false;
    disconnect(timeout);
    return true;
}
void Store::add_row(uint32_t ticket, const Row& row)
{
    if (!active(ticket)) return;
    auto& result = *current();
    if (result.info.total < UINT16_MAX) ++result.info.total;
    if (result.info.count < kRows) result.rows[result.info.count++] = row;
    else result.info.truncated = true;
}
bool Store::append(uint32_t ticket, uint16_t offset, const uint8_t* data, size_t length)
{
    if (!active(ticket)) return false;
    auto& result = *current();
    if (offset != result.info.length) {
        finish(ticket, invalid_request);
        return false;
    }
    if (length > kValueBytes - offset) {
        finish(ticket, too_large);
        return false;
    }
    if (length) std::memcpy(result.value.data() + offset, data, length);
    result.info.length += length;
    return true;
}
ResultPage Store::result(uint32_t ticket, size_t index, size_t offset) const
{
    for (const auto& result : results_) if (ticket && result.info.ticket == ticket) {
        auto page = result.info;
        // Publish complete immutable values; never mix partially received data
        // from different callback invocations in a paginated response.
        if (page.state != State::complete) return page;
        if (index < page.count) page.row = result.rows[index];
        if (offset < page.length) {
            page.bytes = std::min(kPageBytes, page.length - offset);
            std::memcpy(page.data, result.value.data() + offset, page.bytes);
        }
        return page;
    }
    return {};
}
void Store::notify(uint16_t handle, bool indication, const uint8_t* data, size_t length)
{
    if (!status_.connected) return;
    // Never alias an old cursor. After sequence exhaustion reconnect to begin
    // a new generation; retained events remain available until then.
    if (status_.latest_event == UINT32_MAX) {
        if (status_.dropped_events != UINT32_MAX) ++status_.dropped_events;
        return;
    }
    auto& event = events_[next_event_++ % kEvents];
    if (event.info.sequence) ++status_.dropped_events;
    event = {};
    event.info.sequence = ++status_.latest_event;
    event.info.generation = status_.generation;
    event.info.handle = handle; event.info.indication = indication;
    event.info.length = std::min(length, kValueBytes);
    event.info.truncated = length > kValueBytes;
    if (event.info.length) std::memcpy(event.value.data(), data, event.info.length);
}
EventPage Store::event(uint32_t after, uint32_t sequence, size_t offset) const
{
    const Event* selected = nullptr;
    for (const auto& event : events_) {
        if (!event.info.sequence) continue;
        if (sequence ? event.info.sequence == sequence : event.info.sequence > after &&
            (!selected || event.info.sequence < selected->info.sequence)) selected = &event;
    }
    if (!selected) return {};
    auto page = selected->info;
    if (!sequence) page.lost = page.sequence - after - 1;
    if (offset < page.length) {
        page.bytes = std::min(kPageBytes, page.length - offset);
        std::memcpy(page.data, selected->value.data() + offset, page.bytes);
    }
    return page;
}
} // namespace connectivity::gateway
