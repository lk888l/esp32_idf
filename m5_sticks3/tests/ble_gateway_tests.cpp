#include "ble_gateway.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace connectivity::gateway;
namespace {
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
Request request(Store& store, Operation operation = Operation::read)
{
    Request value{}; value.generation = store.status().generation;
    value.operation = operation; value.handle = 7; return value;
}
void lifecycle()
{
    Store store;
    Request r{}; r.generation = 1;
    check(store.reserve(r, 1) == disconnected, "offline admission");
    store.connect(); r = request(store);
    check(store.reserve(r, 1) == none && store.reserve(r, 2) == busy, "single outstanding operation");
    check(store.start(1, 100), "accepted ticket starts");
    const uint8_t bytes[] = {0, 0xff, 2};
    check(store.append(1, 0, bytes, sizeof(bytes)), "binary value including NUL");
    check(store.result(1, 0, 0).bytes == 0, "partial value not published");
    store.disconnect();
    check(store.result(1, 0, 0).error == disconnected, "disconnect completes pending result");
    store.connect();
    check(store.reserve(r, 2) == stale_generation, "old connection generation rejected");
    r = request(store);
    check(store.reserve(r, 2) == none && store.start(2, 100), "new peer accepted");
    store.finish(1, 0);
    check(store.active(2) && !store.append(1, 0, bytes, sizeof(bytes)), "late callback cannot finish or corrupt new request");
    check(!store.expire(100 + kTimeoutUs - 1) && store.expire(100 + kTimeoutUs), "deadline boundary");
    check(store.result(2, 0, 0).error == timeout && !store.status().connected, "timeout closes admission");
}
void bounds()
{
    Store store; store.connect();
    auto r = request(store);
    uint8_t bytes[kValueBytes];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = i;
    check(store.reserve(r, 1) == none, "reserve read");
    check(store.append(1, 0, bytes, 256) && store.append(1, 256, bytes + 256, 256), "full ATT value");
    store.finish(1, 0);
    for (size_t offset = 0; offset < sizeof(bytes); offset += kPageBytes) {
        const auto page = store.result(1, 0, offset);
        check(page.bytes == kPageBytes && std::memcmp(page.data, bytes + offset, kPageBytes) == 0, "lossless pagination");
    }
    check(store.result(1, 0, SIZE_MAX).bytes == 0, "out-of-bounds query has no data");
    check(store.reserve(r, 2) == none && store.append(2, 0, bytes, sizeof(bytes)), "second read");
    check(!store.append(2, 512, bytes, 1) && store.result(2, 0, 0).error == too_large, "oversized value fails explicitly");
    check(store.reserve(r, 3) == none && !store.append(3, 65535, bytes, 1), "bad callback offset fails");
    r.operation = Operation::services;
    check(store.reserve(r, 4) == none, "discover");
    Row row{};
    for (unsigned i = 0; i < kRows + 3; ++i) { row.handle = i + 1; store.add_row(4, row); }
    store.finish(4, 0);
    const auto page = store.result(4, kRows - 1, 0);
    check(page.count == kRows && page.total == kRows + 3 && page.truncated && page.row.handle == kRows, "discovery overflow reported");
    for (uint32_t ticket = 5; ticket < 9; ++ticket) {
        check(store.reserve(r, ticket) == none, "rotate history"); store.finish(ticket, 0);
    }
    check(!store.result(4, 0, 0).ticket && store.result(8, 0, 0).ticket == 8, "bounded history expires old receipts");
}
void events()
{
    Store store; store.connect();
    uint8_t bytes[kValueBytes + 1]{};
    bytes[64] = 0xff;
    for (unsigned i = 0; i < kEvents + 2; ++i) store.notify(9, i % 2, bytes, sizeof(bytes));
    auto first = store.event(0, 0, 0);
    check(first.sequence == 3 && first.lost == 2 && first.length == kValueBytes && first.truncated, "overflow and truncation visible");
    check(store.status().dropped_events == 2, "bounded event ring");
    check(store.event(0, first.sequence, 64).data[0] == 0xff, "same event pages retain bytes");
    check(store.event(0, 0, 0).sequence == first.sequence, "read does not consume another client's cursor");
    check(!store.event(0, 1, 0).sequence, "overwritten exact event is unavailable");
    check(!store.event(kEvents + 2, 0, 0).sequence, "caught-up cursor");
    store.disconnect();
    check(!store.event(0, 0, 0).sequence, "disconnect clears notifications");
    store.connect(); store.notify(4, true, bytes, 0);
    check(store.event(0, 0, 0).indication && store.event(0, 0, 0).length == 0, "empty indication supported");
}
void validation()
{
    uint8_t bytes[kWriteBytes]{}, length = 0;
    check(decode_hex("00aAFF", bytes, sizeof(bytes), length) && length == 3 && bytes[2] == 255, "binary hex");
    char text[7]{}; encode_hex(bytes, 3, text);
    check(std::strcmp(text, "00aaff") == 0, "canonical hex");
    check(!decode_hex("a", bytes, sizeof(bytes), length) && !decode_hex("0x", bytes, sizeof(bytes), length), "bad hex");
    check(!decode_hex("0000", bytes, 1, length), "write capacity");
    check(decode_hex("", bytes, sizeof(bytes), length) && length == 0, "zero-length write");
    Request r{}; r.generation = 1; r.operation = Operation::descriptors; r.handle = r.end = 1;
    check(!valid(r), "descriptor range excludes characteristic value handle");
    r.end = 2; check(valid(r), "descriptor range");
    r.operation = Operation::subscribe; r.mode = 3; check(!valid(r), "invalid CCCD mode");
}
}
int main()
{
    lifecycle(); bounds(); events(); validation();
    std::cout << "BLE gateway state, pagination, overflow and generation tests passed\n";
}
