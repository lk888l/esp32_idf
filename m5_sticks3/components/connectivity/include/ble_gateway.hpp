#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace connectivity::gateway {

constexpr size_t kWriteBytes = 48;
constexpr size_t kValueBytes = 512;
constexpr size_t kPageBytes = 64;
constexpr size_t kRows = 16;
constexpr size_t kResults = 4;
constexpr size_t kEvents = 8;
constexpr int64_t kTimeoutUs = 15'000'000;

enum class Operation : uint8_t { services, characteristics, descriptors, read, write, subscribe, mtu, pair };
enum class State : uint8_t { queued, running, complete };
// Negative errors belong to the gateway; positive errors are NimBLE statuses.
enum Error : int32_t {
    none = 0, disconnected = -1, stale_generation = -2, busy = -3,
    invalid_request = -4, timeout = -5, too_large = -6, transport_busy = -7,
};
struct Request {
    Operation operation = Operation::services;
    uint32_t generation = 0;
    uint16_t handle = 0, end = 0;
    uint8_t mode = 0, length = 0;
    uint8_t data[kWriteBytes]{};
};
struct Row {
    char uuid[40]{};
    uint16_t handle = 0, end = 0, value_handle = 0;
    uint8_t properties = 0;
};
struct Status {
    bool connected = false;
    uint32_t generation = 0, active_ticket = 0;
    uint32_t latest_event = 0, dropped_events = 0;
};
struct ResultPage {
    uint32_t ticket = 0, generation = 0;
    Operation operation = Operation::services;
    State state = State::queued;
    int32_t error = 0;
    uint16_t handle = 0, total = 0, length = 0;
    uint8_t count = 0, bytes = 0;
    bool truncated = false;
    Row row{};
    uint8_t data[kPageBytes]{};
};
struct EventPage {
    uint32_t sequence = 0, generation = 0, lost = 0;
    uint16_t handle = 0, length = 0;
    uint8_t bytes = 0;
    bool indication = false, truncated = false;
    uint8_t data[kPageBytes]{};
};

bool valid(const Request& request);
bool decode_hex(std::string_view text, uint8_t* output, size_t capacity, uint8_t& length);
void encode_hex(const uint8_t* input, size_t length, char* output);
const char* operation_name(Operation operation);
const char* error_name(int32_t error);

// Portable bounded state machine. Its owner serializes ALL access. No drivers,
// heap allocation, callbacks or blocking work are permitted inside this model.
class Store {
public:
    void connect();
    void disconnect(int32_t error = Error::disconnected);
    Status status() const { return status_; }
    int32_t reserve(const Request& request, uint32_t ticket);
    bool start(uint32_t ticket, int64_t now);
    bool active(uint32_t ticket) const;
    void finish(uint32_t ticket, int32_t error);
    bool expire(int64_t now);
    void add_row(uint32_t ticket, const Row& row);
    bool append(uint32_t ticket, uint16_t offset, const uint8_t* data, size_t length);
    ResultPage result(uint32_t ticket, size_t index, size_t offset) const;
    void notify(uint16_t handle, bool indication, const uint8_t* data, size_t length);
    EventPage event(uint32_t after, uint32_t sequence, size_t offset) const;
private:
    struct Result {
        ResultPage info{};
        std::array<Row, kRows> rows{};
        std::array<uint8_t, kValueBytes> value{};
    };
    struct Event {
        EventPage info{};
        std::array<uint8_t, kValueBytes> value{};
    };
    Result* current();
    Status status_{};
    std::array<Result, kResults> results_{};
    std::array<Event, kEvents> events_{};
    size_t next_result_ = 0, next_event_ = 0;
    int64_t deadline_ = 0;
};
} // namespace connectivity::gateway
