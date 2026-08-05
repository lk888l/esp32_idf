#include "canopen/standard_profile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

class FakeTransport final : public can::ITransport {
public:
    can::SendResult send(const can::Frame& frame, uint32_t) override
    {
        frames.push_back(frame);
        return can::SendResult::ok;
    }
    std::vector<can::Frame> frames;
};

class FakeParameterStorage final : public canopen::ParameterStorage {
public:
    bool store_node_id(uint8_t node_id) override
    {
        ++store_calls;
        last_node_id = node_id;
        return succeed;
    }

    bool succeed = true;
    uint8_t last_node_id = 0;
    uint32_t store_calls = 0;
};

[[noreturn]] void fail(const char* expression, int line)
{
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

can::Frame sdo_request(uint8_t node, std::array<uint8_t, 8> payload)
{
    can::Frame frame{};
    frame.id = canopen::cob::rsdo + node;
    frame.size = 8;
    std::copy(payload.begin(), payload.end(), frame.data.begin());
    return frame;
}

can::Frame nmt(uint8_t command, uint8_t node)
{
    can::Frame frame{};
    frame.id = canopen::cob::nmt;
    frame.size = 2;
    frame.data[0] = command;
    frame.data[1] = node;
    return frame;
}

std::array<uint8_t, 4> le32(uint32_t value)
{
    return {static_cast<uint8_t>(value),
            static_cast<uint8_t>(value >> 8U),
            static_cast<uint8_t>(value >> 16U),
            static_cast<uint8_t>(value >> 24U)};
}

void od_write_u32(canopen::ObjectDictionary& dictionary,
                  uint16_t index,
                  uint8_t subindex,
                  uint32_t value)
{
    const auto data = le32(value);
    CHECK(dictionary.write({index, subindex}, data) == canopen::AbortCode::none);
}

void od_write_u16(canopen::ObjectDictionary& dictionary,
                  uint16_t index,
                  uint8_t subindex,
                  uint16_t value)
{
    const std::array<uint8_t, 2> data{static_cast<uint8_t>(value),
                                      static_cast<uint8_t>(value >> 8U)};
    CHECK(dictionary.write({index, subindex}, data) == canopen::AbortCode::none);
}

void od_write_u8(canopen::ObjectDictionary& dictionary,
                 uint16_t index,
                 uint8_t subindex,
                 uint8_t value)
{
    CHECK(dictionary.write({index, subindex}, std::span<const uint8_t>(&value, 1)) ==
          canopen::AbortCode::none);
}

void test_nmt_heartbeat_and_sdo()
{
    constexpr uint8_t node_id = 0x21;
    FakeTransport transport;
    canopen::ProfileConfig config{};
    config.node_id = node_id;
    config.producer_heartbeat_ms = 100;
    config.identity = {0x00686578, 0xC5000001, 0x00010000, 0x12345678};
    config.device_name = "ESP32-C5 segmented SDO";
    canopen::StandardProfile profile(config, transport);
    std::array<uint8_t, 10> writable_blob{};
    CHECK(profile.dictionary().add_bytes({0x2101, 0},
                                         canopen::DataType::octet_string,
                                         writable_blob,
                                         canopen::Access::read_write) ==
          canopen::AbortCode::none);
    CHECK(profile.initialize() == canopen::AbortCode::none);
    CHECK(profile.start(0));
    CHECK(transport.frames.size() == 1);
    CHECK(transport.frames.back().id == 0x721);
    CHECK(transport.frames.back().data[0] == 0x00);

    profile.process(99'000);
    CHECK(transport.frames.size() == 1);
    profile.process(100'000);
    CHECK(transport.frames.back().data[0] == 0x7F);

    CHECK(profile.handle(nmt(0x01, node_id), 101'000));
    CHECK(profile.node().state() == canopen::NmtState::operational);
    CHECK(transport.frames.back().data[0] == 0x05);

    transport.frames.clear();
    CHECK(profile.handle(sdo_request(node_id, {0x40, 0x18, 0x10, 0x01, 0, 0, 0, 0}),
                         102'000));
    CHECK(transport.frames.size() == 1);
    CHECK(transport.frames[0].id == 0x5A1);
    CHECK(transport.frames[0].data[0] == 0x43);
    const auto vendor_id = le32(0x00686578);
    CHECK(std::equal(
        vendor_id.begin(), vendor_id.end(), transport.frames[0].data.begin() + 4));

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(node_id, {0x23, 0x00, 0x20, 0x04, 0x78, 0x56, 0x34, 0x12}), 103'000));
    CHECK(transport.frames[0].data[0] == 0x60);
    CHECK(profile.application_value() == 0x12345678);

    transport.frames.clear();
    CHECK(profile.handle(sdo_request(node_id, {0x40, 0x08, 0x10, 0, 0, 0, 0, 0}),
                         104'000));
    CHECK(transport.frames.back().data[0] == 0x41);
    std::string uploaded;
    bool toggle = false;
    while (true) {
        const std::size_t before = transport.frames.size();
        std::array<uint8_t, 8> request{};
        request[0] = static_cast<uint8_t>(0x60U | (toggle ? 0x10U : 0U));
        CHECK(profile.handle(sdo_request(node_id, request), 105'000 + before));
        const auto& response = transport.frames.back();
        const std::size_t count = 7U - ((response.data[0] >> 1U) & 0x07U);
        uploaded.append(reinterpret_cast<const char*>(response.data.data() + 1), count);
        const bool complete = (response.data[0] & 1U) != 0;
        if (complete) {
            break;
        }
        toggle = !toggle;
    }
    CHECK(uploaded == config.device_name);

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(node_id, {0x21, 0x01, 0x21, 0, 10, 0, 0, 0}), 110'000));
    CHECK(transport.frames.back().data[0] == 0x60);
    CHECK(profile.handle(
        sdo_request(node_id, {0x00, 1, 2, 3, 4, 5, 6, 7}), 111'000));
    CHECK(transport.frames.back().data[0] == 0x20);
    CHECK(profile.handle(
        sdo_request(node_id, {0x19, 8, 9, 10, 0, 0, 0, 0}), 112'000));
    CHECK(transport.frames.back().data[0] == 0x30);
    const std::array<uint8_t, 10> expected{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(writable_blob == expected);
}

void test_node_id_configuration_and_store()
{
    constexpr uint8_t active_node_id = 0x21;
    FakeTransport transport;
    FakeParameterStorage storage;
    canopen::ProfileConfig config{};
    config.node_id = active_node_id;
    config.parameter_storage = &storage;
    canopen::StandardProfile profile(config, transport);
    CHECK(profile.initialize() == canopen::AbortCode::none);
    CHECK(profile.start(0));

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(active_node_id, {0x40, 0x10, 0x10, 0x01, 0, 0, 0, 0}), 1'000));
    CHECK(transport.frames.back().data[0] == 0x43);
    CHECK(transport.frames.back().data[4] == 1);

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(active_node_id, {0x2F, 0x01, 0x20, 0x01, 0, 0, 0, 0}), 2'000));
    CHECK(transport.frames.back().data[0] == 0x80);
    const auto range_abort = le32(static_cast<uint32_t>(canopen::AbortCode::value_range));
    CHECK(std::equal(
        range_abort.begin(), range_abort.end(), transport.frames.back().data.begin() + 4));
    CHECK(profile.configured_node_id() == active_node_id);

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(active_node_id, {0x2F, 0x01, 0x20, 0x01, 0x22, 0, 0, 0}), 3'000));
    CHECK(transport.frames.back().data[0] == 0x60);
    CHECK(profile.configured_node_id() == 0x22);
    CHECK(profile.node().node_id() == active_node_id);
    CHECK(!profile.handle(
        sdo_request(0x22, {0x40, 0x18, 0x10, 0x01, 0, 0, 0, 0}), 4'000));

    transport.frames.clear();
    CHECK(profile.handle(
        sdo_request(active_node_id, {0x23, 0x10, 0x10, 0x01, 0, 0, 0, 0}), 5'000));
    CHECK(transport.frames.back().data[0] == 0x80);
    const auto store_abort = le32(static_cast<uint32_t>(canopen::AbortCode::data_store));
    CHECK(std::equal(
        store_abort.begin(), store_abort.end(), transport.frames.back().data.begin() + 4));
    CHECK(storage.store_calls == 0);

    transport.frames.clear();
    CHECK(profile.handle(sdo_request(active_node_id,
                                     {0x23, 0x10, 0x10, 0x01, 's', 'a', 'v', 'e'}),
                         6'000));
    CHECK(transport.frames.back().data[0] == 0x60);
    CHECK(storage.store_calls == 1);
    CHECK(storage.last_node_id == 0x22);

    transport.frames.clear();
    profile.process(100'000);
    CHECK(transport.frames.back().id == canopen::cob::heartbeat + active_node_id);

    storage.succeed = false;
    CHECK(profile.handle(
        sdo_request(active_node_id, {0x2F, 0x01, 0x20, 0x01, 0x23, 0, 0, 0}), 101'000));
    transport.frames.clear();
    CHECK(profile.handle(sdo_request(active_node_id,
                                     {0x23, 0x10, 0x10, 0x01, 's', 'a', 'v', 'e'}),
                         102'000));
    CHECK(transport.frames.back().data[0] == 0x80);
    CHECK(std::equal(
        store_abort.begin(), store_abort.end(), transport.frames.back().data.begin() + 4));
}

void test_fd_pdo_mapping()
{
    constexpr uint8_t node_id = 0x21;
    FakeTransport transport;
    canopen::ProfileConfig config{};
    config.node_id = node_id;
    canopen::StandardProfile profile(config, transport);
    uint32_t value_a = 0x11111111;
    uint32_t value_b = 0x22222222;
    uint32_t value_c = 0x33333333;
    CHECK(profile.dictionary().add_scalar(
              {0x2100, 1}, value_a, canopen::Access::read_write, true) ==
          canopen::AbortCode::none);
    CHECK(profile.dictionary().add_scalar(
              {0x2100, 2}, value_b, canopen::Access::read_write, true) ==
          canopen::AbortCode::none);
    CHECK(profile.dictionary().add_scalar(
              {0x2100, 3}, value_c, canopen::Access::read_write, true) ==
          canopen::AbortCode::none);
    CHECK(profile.initialize() == canopen::AbortCode::none);
    auto& od = profile.dictionary();

    od_write_u32(od, 0x1400, 1, 0x80000221);
    od_write_u8(od, 0x1600, 0, 0);
    od_write_u32(od, 0x1600, 1, 0x21000120);
    od_write_u32(od, 0x1600, 2, 0x21000220);
    od_write_u32(od, 0x1600, 3, 0x21000320);
    od_write_u8(od, 0x1600, 0, 3);
    od_write_u32(od, 0x1400, 1, 0x00000221);

    od_write_u32(od, 0x1800, 1, 0xC00001A1);
    od_write_u8(od, 0x1A00, 0, 0);
    od_write_u32(od, 0x1A00, 1, 0x21000120);
    od_write_u32(od, 0x1A00, 2, 0x21000220);
    od_write_u32(od, 0x1A00, 3, 0x21000320);
    od_write_u8(od, 0x1A00, 0, 3);
    od_write_u8(od, 0x1800, 2, 255);
    od_write_u16(od, 0x1800, 5, 10);
    od_write_u32(od, 0x1800, 1, 0x400001A1);

    CHECK(profile.start(0));
    CHECK(profile.handle(nmt(0x01, node_id), 1'000));
    can::Frame rpdo{};
    rpdo.id = 0x221;
    rpdo.size = 12;
    rpdo.fd = true;
    rpdo.bitrate_switch = true;
    const auto a = le32(0xAABBCCDD);
    const auto b = le32(0x12345678);
    const auto c = le32(0xDEADBEEF);
    std::copy(a.begin(), a.end(), rpdo.data.begin());
    std::copy(b.begin(), b.end(), rpdo.data.begin() + 4);
    std::copy(c.begin(), c.end(), rpdo.data.begin() + 8);
    CHECK(profile.handle(rpdo, 2'000));
    CHECK(value_a == 0xAABBCCDD);
    CHECK(value_b == 0x12345678);
    CHECK(value_c == 0xDEADBEEF);

    transport.frames.clear();
    profile.process(12'000);
    CHECK(transport.frames.size() == 1);
    const auto& tpdo = transport.frames[0];
    CHECK(tpdo.id == 0x1A1);
    CHECK(tpdo.fd && tpdo.bitrate_switch && tpdo.size == 12);
    CHECK(std::equal(a.begin(), a.end(), tpdo.data.begin()));
    CHECK(std::equal(b.begin(), b.end(), tpdo.data.begin() + 4));
    CHECK(std::equal(c.begin(), c.end(), tpdo.data.begin() + 8));
}

} // namespace

int main()
{
    test_nmt_heartbeat_and_sdo();
    test_node_id_configuration_and_store();
    test_fd_pdo_mapping();
    std::puts("All CANopen core tests passed.");
    return 0;
}
