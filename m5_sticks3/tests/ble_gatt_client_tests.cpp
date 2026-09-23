#include "ble_gatt_client.hpp"
#include "platform.hpp"
#include <cstdlib>
#include <iostream>
using namespace connectivity::gateway;
namespace {
void check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
Request request(Client& client, Operation op)
{
    Request r{}; r.operation = op; r.generation = client.status().generation;
    r.handle = 7; r.end = 20; return r;
}
void start(Client& client, const Request& r, uint32_t ticket)
{
    check(client.reserve(r, ticket) == 0, "reserve");
    client.execute(r, ticket);
}
}
int main()
{
    Client client; client.connected(12);
    auto r = request(client, Operation::write); r.length = 21;
    start(client, r, 1);
    check(gatt_fake::writes == 0 && client.result(1, 0, 0).error == too_large, "MTU overflow never writes prefix");
    r.length = 20; r.data[0] = 0xff; start(client, r, 2);
    check(gatt_fake::written.size() == 20 && gatt_fake::written[0] == 0xff, "write binary");
    ble_gatt_error success{}, done{BLE_HS_EDONE, 0};
    auto old_callback = gatt_fake::attribute; auto old_argument = gatt_fake::argument;
    client.disconnected(); client.connected(12);
    r = request(client, Operation::read); start(client, r, 3);
    check(old_callback(12, &success, nullptr, old_argument) == BLE_HS_EDONE &&
          client.status().active_ticket == 3, "late callback with reused HCI handle ignored");
    os_mbuf value{{0, 1, 255}};
    ble_gatt_attr attr{7, 0, &value};
    {
        Client inactive;
        check(gatt_fake::attribute(12, &success, &attr, gatt_fake::argument) == 0, "inactive instance cannot steal callbacks");
    }
    gatt_fake::attribute(12, &done, nullptr, gatt_fake::argument);
    check(client.result(3, 0, 0).length == 3 && client.result(3, 0, 0).data[2] == 255, "long read completion");

    r = request(client, Operation::services); start(client, r, 4);
    ble_gatt_svc service{1, 20, {}};
    gatt_fake::service(12, &success, &service, gatt_fake::argument);
    gatt_fake::service(12, &done, nullptr, gatt_fake::argument);
    check(client.result(4, 0, 0).row.end == 20, "service range retained");
    r = request(client, Operation::characteristics); start(client, r, 5);
    ble_gatt_chr chr{6, 7, 0x12, {}};
    gatt_fake::characteristic(12, &success, &chr, gatt_fake::argument);
    gatt_fake::characteristic(12, &done, nullptr, gatt_fake::argument);
    check(client.result(5, 0, 0).row.value_handle == 7, "characteristic value handle");
    r = request(client, Operation::descriptors); start(client, r, 6);
    ble_gatt_dsc descriptor{10, {}};
    gatt_fake::descriptor(12, &success, 7, &descriptor, gatt_fake::argument);
    gatt_fake::descriptor(12, &done, 7, nullptr, gatt_fake::argument);
    check(client.result(6, 0, 0).row.handle == 10, "CCCD need not follow value handle");

    r = request(client, Operation::subscribe); r.handle = 10; r.mode = 2; start(client, r, 7);
    check(gatt_fake::last_handle == 10 && gatt_fake::written == std::vector<uint8_t>({2, 0}), "indication subscription uses explicit CCCD");
    gatt_fake::attribute(12, &success, nullptr, gatt_fake::argument);
    client.notification(99, 7, true, &value);
    check(!client.event(0, 0, 0).sequence, "control-link notification excluded");
    client.notification(12, 7, true, &value);
    check(client.event(0, 0, 0).indication && client.event(0, 0, 0).bytes == 3, "peer indication forwarded");

    r = request(client, Operation::mtu); start(client, r, 8);
    gatt_fake::mtu_callback(12, &success, 259, gatt_fake::argument);
    check(client.result(8, 0, 0).data[0] == 3 && client.result(8, 0, 0).data[1] == 1, "MTU result");
    r = request(client, Operation::pair); start(client, r, 9);
    client.security_complete(99, 0);
    check(client.status().active_ticket == 9, "wrong security event ignored");
    client.security_complete(12, 0);
    check(client.result(9, 0, 0).state == State::complete, "pairing completion");
    r = request(client, Operation::read); gatt_fake::result = 5; start(client, r, 10);
    check(client.result(10, 0, 0).error == 5, "synchronous stack errors preserved");
    gatt_fake::result = 0; start(client, r, 11);
    check(client.expired(fake::now + kTimeoutUs) && client.result(11, 0, 0).error == timeout, "timeout requests link termination");
    check(client.reserve(r, 12) == disconnected, "timeout closes admission");
    std::cout << "NimBLE gateway callback, lifecycle and driver-boundary tests passed\n";
}
