#include "connectivity_service.hpp"
#include "connectivity_runtime.hpp"
#include "local_console_protocol.hpp"
#include "wifi_transport.hpp"
#include "ble_transport.hpp"
#include "sdkconfig.h"
#include "../components/connectivity/src/serial_console.hpp"
#include "cJSON.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

// Replace physical IO only. The real Service, parser, queue, memory policy,
// NVS codec and JSON library are linked into this executable unchanged.
namespace connectivity {
esp_err_t SerialConsole::start_console(RequestHandler handler, void* context)
{ fake::serial = handler; fake::context = context; return ESP_OK; }
esp_err_t SerialConsole::stop_console() { fake::serial = nullptr; return ESP_OK; }
void SerialConsole::main() {}
}
namespace {
using namespace connectivity;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "Service integration: " << message << '\n'; std::exit(1); }
}
struct Reply {
    std::string text;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json{nullptr, cJSON_Delete};
    explicit Reply(const char* data) : text(data), json(cJSON_Parse(data), cJSON_Delete)
    { check(json != nullptr, "response is complete valid JSON"); }
    double number(const char* key) const { auto* item = cJSON_GetObjectItemCaseSensitive(json.get(), key); check(cJSON_IsNumber(item), key); return item->valuedouble; }
    std::string string(const char* key) const { auto* item = cJSON_GetObjectItemCaseSensitive(json.get(), key); check(cJSON_IsString(item), key); return item->valuestring; }
    bool ok() const { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(), "ok")); }
};
Reply invoke(const char* input, RequestHandler handler = nullptr, size_t capacity = kMaxResponseBytes)
{
    char output[kMaxResponseBytes]{};
    (handler ? handler : fake::serial)(fake::context, input, std::strlen(input), output, capacity);
    return Reply(output);
}
Reply cli(const char* input)
{
    char request[kMaxRequestBytes + 1]{};
    check(!console::compile(input, 17, request, sizeof(request)), "CLI compiles");
    return invoke(request);
}
uint32_t submit(const char* command)
{
    const auto result = cli(command);
    if (!result.ok()) std::cerr << result.text << '\n';
    check(result.ok() && result.string("result") == "accepted", "mutation accepted");
    return static_cast<uint32_t>(result.number("ticket"));
}
void wifi_up(Service& service)
{
    fake::wifi.state = WifiState::connected; std::strcpy(fake::wifi.address, "192.0.2.2"); service.process();
}
void ble_up(Service& service)
{
    fake::ble_diag.connecting = false; fake::ble_diag.peer_connected = true;
    ++fake::ble_diag.peer_generation;
    std::strcpy(fake::ble_diag.peer_identity, fake::ble_diag.peer_address);
    fake::ble_diag.peer_identity_type = fake::ble_diag.peer_address_type;
    service.process();
}
void seed_legacy()
{
    struct Legacy { uint32_t version = 1; WifiCredentials wifi{}; char token[33]{}; char ap_password[17]{}; } legacy;
    static_assert(sizeof(Legacy) == 152);
    std::strcpy(legacy.wifi.ssid, "legacy-home"); std::strcpy(legacy.wifi.password, "legacy-password");
    std::memset(legacy.token, '1', 32); std::memset(legacy.ap_password, '2', 16);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&legacy);
    fake::nvs["settings"] = {bytes, bytes + sizeof(legacy)};
}
void common_checks(Service& service)
{
    check(cli("status").string("error") == "unavailable", "no commands before lifecycle fully starts");
    service.process();
    for (const auto* command : {"help", "help wifi", "help ble", "help protocol", "status", "capabilities", "wifi status", "ble status", "wifi saved", "ble saved", "ble bonds", "traffic"})
        check(cli(command).ok(), "documented query succeeds");
    for (const auto* topic : {"wifi", "ble", "protocol"}) {
        const auto request = std::string("{\"v\":1,\"id\":1,\"op\":\"help\",\"topic\":\"") + topic + "\"}";
        check(invoke(request.c_str(), nullptr, 512).ok(), "help fits BLE ATT response capacity");
    }
    check(cli("wifi saved").text.find("legacy-password") == std::string::npos, "saved query excludes passwords");
    check(invoke(R"({"v":1,"id":1,"op":"wifi.forget","all":true,"slot":0})").string("error") == "invalid_slot", "ambiguous destructive target rejected");
    check(invoke(R"({"v":1,"id":1,"op":"wifi.connect","ssid":"a","password":"password","remember":"false"})").string("error") == "invalid_remember", "strict boolean");
    check(invoke(R"({"v":1,"id":1,"op":"wifi.disable","origin":"serial"})").string("error") == "unknown_field", "unknown mutation field rejected");
    check(invoke(R"({"v":1,"id":1,"op":"wifi.disable","op":"wifi.enable"})").string("error") == "invalid_json", "duplicate operation rejected");
    check(invoke(R"({"v":1,"id":1,"op":"wifi.connect","ssid":"a\u0000b","password":"password"})").string("error") == "invalid_json", "escaped NUL rejected");
    check(cli("command 4294967295").string("error") == "unknown_ticket", "unknown results are explicit");
}
[[maybe_unused]] void radio_checks(Service& service)
{
    check(std::string(fake::credentials.ssid) == "legacy-home", "legacy saved network restored on boot");
    const auto migrated = fake::nvs.at("radio_mem");
    RadioMemory memory;
    check(decode_memory(migrated.data(), migrated.size(), memory) && std::string(memory.wifi[0].ssid) == "legacy-home", "legacy credentials migrated");
    check(fake::nvs.at("settings")[4] == 0, "legacy plaintext credential copy retired");
    const auto commits = fake::commits;
    const auto first = submit("wifi connect home password"); service.process();
    check(command_result(first).applied && command_result(first).error == 0, "receipt reports driver application");
    check(fake::commits == commits && memory_snapshot().wifi_pending, "association start does not commit credentials");
    fake::association_matches = false;
    wifi_up(service);
    check(fake::commits == commits && memory_snapshot().wifi_pending, "stale IP event cannot authorize persistence");
    fake::association_matches = true;
    wifi_up(service);
    check(!memory_snapshot().wifi_pending && std::string(memory_snapshot().wifi_ssids[1]) == "home", "DHCP success commits credentials");
    const auto after_save = fake::commits;
    submit("wifi remember"); service.process();
    check(fake::commits == after_save, "identical memory avoids flash wear");
    submit("wifi connect guest password --temporary"); service.process(); wifi_up(service);
    check(fake::commits == after_save && cli("wifi saved").number("count") == 2, "temporary connection does not persist");
    submit("wifi remember"); service.process();
    check(cli("wifi saved").number("count") == 3, "remember current connected network");
    submit("wifi use 0"); service.process(); wifi_up(service);
    check(std::string(fake::credentials.ssid) == "legacy-home" && memory_snapshot().preferred_wifi == 0, "saved switch restores credentials");
    submit("wifi connect pending password"); service.process();
    submit("wifi forget all"); service.process(); wifi_up(service);
    check(cli("wifi saved").number("count") == 0 && !memory_snapshot().wifi_pending, "forget cancels pending save");
    submit("wifi remember"); service.process();
    check(cli("wifi saved").number("count") == 1, "live connection survives forget until explicitly disconnected");
    submit("wifi connect timeout password"); service.process(); fake::now += 120'000'001; service.process();
    check(memory_snapshot().wifi_error == ESP_ERR_TIMEOUT && !memory_snapshot().wifi_pending, "bounded remember deadline");
    submit("wifi connect storage-error password"); service.process(); fake::write_error = ESP_FAIL; wifi_up(service);
    check(memory_snapshot().wifi_error == ESP_FAIL && cli("wifi saved").number("count") == 1, "NVS error is observable without replacing saved memory");
    fake::write_error = 0;
    submit("wifi remember"); service.process(); check(memory_snapshot().wifi_error == 0, "explicit save retry recovers");

    submit("ble connect AA:BB:CC:DD:EE:FF public"); service.process();
    check(memory_snapshot().ble_pending && cli("ble saved").number("count") == 0, "BLE link start is not persisted");
    ble_up(service); check(cli("ble saved").number("count") == 1, "BLE connected identity persisted");
    submit("ble connect 4A:BB:CC:DD:EE:FF random"); service.process(); ble_up(service);
    check(memory_snapshot().ble_error == ESP_ERR_NOT_SUPPORTED && cli("ble saved").number("count") == 1, "private address cannot overwrite stable identity");
    submit("ble use 0"); service.process(); ble_up(service);
    check(std::string(fake::ble_diag.peer_address) == "AA:BB:CC:DD:EE:FF", "saved BLE identity reused");
    submit("ble connect CA:BB:CC:DD:EE:FF random"); service.process();
    submit("ble forget all"); service.process(); ble_up(service);
    check(cli("ble saved").number("count") == 0, "forget wins over later BLE connect callback");

    const auto unauthorized = invoke(R"({"v":1,"id":1,"op":"wifi.disable","origin":"serial"})", fake::http);
    check(unauthorized.string("error") == "unauthorized", "wire input cannot spoof trusted USB origin");
    check(invoke(R"({"v":1,"id":1,"op":"ble.forget","all":true})", fake::ble).string("error") == "unauthorized", "BLE memory mutation still needs token");
    const auto old_config = invoke(R"({"v":1,"id":1,"op":"wifi.configure","token":"11111111111111111111111111111111","ssid":"legacy-api","password":"password"})", fake::http);
    check(old_config.ok(), "legacy authenticated configure accepted"); service.process();
    check(cli("wifi saved").number("count") == 3, "legacy configure preserves immediate-persistence semantics");
    uint32_t tickets[4]{};
    for (auto& ticket : tickets) ticket = submit("wifi on");
    for (int i = 0; i < 32; ++i) check(cli("wifi off").string("error") == "busy", "bounded queue backpressure");
    service.process(); const auto next = submit("wifi on");
    check(next == tickets[3] + 1, "rejected submissions do not consume receipt slots");
    for (size_t i = 1; i < 4; ++i) check(command_result(tickets[i]).ticket == tickets[i], "pending receipt retained after saturation");
    for (int i = 0; i < 4; ++i) service.process();
}
[[maybe_unused]] void gateway_checks(Service& service)
{
    fake::gatt.connect();
    check(cli("help gatt").ok() && cli("gatt status").number("connected") == 1, "gateway console");
    const auto auth = R"("token":"11111111111111111111111111111111")";
    for (auto handler : {fake::http, fake::ble}) {
        for (const auto* operation : {"status", "result", "events", "read", "write", "subscribe"}) {
            const auto json = std::string(R"({"v":1,"id":1,"op":"ble.gatt.)") + operation + R"("})";
            check(invoke(json.c_str(), handler).string("error") == "unauthorized", "gateway data requires radio auth");
        }
        const auto json = std::string(R"({"v":1,"id":1,"op":"ble.gatt.status",)") + auth + "}";
        check(invoke(json.c_str(), handler, 512).ok(), "authenticated gateway fits BLE response");
    }
    check(cli("gatt write 1 7 zz").string("error") == "invalid_data", "bad hex rejected before queueing");
    check(cli("gatt write 1 7 f").string("error") == "invalid_data", "odd hex rejected");
    check(cli("gatt characteristics 1 7 6").string("error") == "invalid_request", "invalid range");
    check(cli("gatt read 2 7").string("error") == "stale_generation", "wrong generation rejected");
    check(invoke(R"({"v":1,"id":1,"op":"ble.gatt.read","generation":1,"handle":7,"mode":1})").string("error") == "unknown_field", "strict schemas");
    const auto ticket = submit("gatt read 1 7");
    const auto query = std::string("gatt result ") + std::to_string(ticket);
    check(cli(query.c_str()).string("state") == "queued", "service queue visible");
    service.process();
    check(cli("gatt read 1 7").string("error") == "busy", "one ATT procedure at a time");
    fake::gatt.start(ticket, fake::now);
    uint8_t bytes[gateway::kValueBytes]; std::memset(bytes, 0xff, sizeof(bytes));
    fake::gatt.append(ticket, 0, bytes, sizeof(bytes)); fake::gatt.finish(ticket, 0);
    check(cli(query.c_str()).string("data").size() == 128, "long result paginated");
    check(cli((query + " 0 512").c_str()).string("data").empty(), "end offset");
    for (unsigned i = 0; i < gateway::kEvents + 1; ++i) fake::gatt.notify(7, false, bytes, sizeof(bytes));
    check(cli("gatt events 1").number("lost") == 1, "event loss surfaced");
    const auto event_json = std::string(R"({"v":1,"id":1,"op":"ble.gatt.events","generation":1,"sequence":2,"offset":64,)") + auth + "}";
    check(invoke(event_json.c_str(), fake::ble, 512).string("data").size() == 128, "BLE event paging");
    check(invoke(R"({"v":1,"id":1,"op":"ble.gatt.events","generation":1,"sequence":2,"after":0})").string("error") == "invalid_cursor", "ambiguous cursor rejected");
    const auto stale = submit("gatt write 1 7 0100");
    fake::gatt.disconnect(); fake::gatt.connect(); service.process();
    check(command_result(stale).error != ESP_OK && !fake::gatt.status().active_ticket, "queued write cannot cross target switch");
    fake::gatt.disconnect();
}
}
int main()
{
    seed_legacy();
    connectivity::Service service;
    check(service.initialize() == ESP_OK, "initialize");
    common_checks(service);
#if CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
    radio_checks(service);
    gateway_checks(service);
#else
    check(cli("wifi on").string("error") == "disabled", "Wi-Fi compile-out is explicit");
    check(cli("ble on").string("error") == "disabled", "BLE compile-out is explicit");
    check(cli("gatt status").string("error") == "disabled", "gateway compile-out is explicit");
    check(cli("capabilities").number("wifi") == 0 && cli("capabilities").number("ble") == 0, "capabilities reflect compiled transports");
#endif
    check(service.deinitialize() == ESP_OK, "shutdown");
    check(service.initialize() == ESP_OK, "restart preserves versioned NVS"); service.process();
    check(cli("status").ok(), "console recovers after service restart");
    check(service.deinitialize() == ESP_OK, "second shutdown");
    std::cout << "Radio service integration tests passed\n";
}
