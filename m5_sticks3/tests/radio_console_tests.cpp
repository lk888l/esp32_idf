#include "local_console_protocol.hpp"
#include "radio_memory.hpp"
#include "command_history.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace connectivity;
namespace {
void check(bool value, const char* reason)
{
    if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
std::string compile(const char* line)
{
    char json[kMaxRequestBytes + 1]{};
    const auto error = console::compile(line, 7, json, sizeof(json));
    if (error) { std::cerr << "compile: " << error << '\n'; std::exit(1); }
    return json;
}
void rejects(const char* line)
{
    char json[kMaxRequestBytes + 1]{};
    check(console::compile(line, 7, json, sizeof(json)) != nullptr, "invalid command must fail");
}
WifiCredentials wifi(const char* name)
{
    WifiCredentials value{};
    std::strcpy(value.ssid, name); std::strcpy(value.password, "test-pass"); return value;
}
void test_commands()
{
    check(compile("wifi connect \"My network\" \"password !\"") ==
        R"({"v":1,"id":7,"op":"wifi.connect","ssid":"My network","password":"password !","remember":true})", "quoted Wi-Fi command");
    check(compile("wifi connect '开放网络' '' --temporary").find(R"("password":"","remember":false)") != std::string::npos,
          "Unicode SSID and explicit empty password");
    check(compile(R"(wifi connect "a\"b" "abc\\defg")").find(R"("ssid":"a\"b","password":"abc\\defg")") != std::string::npos,
          "quotes and backslashes are JSON-escaped");
    check(compile("ble connect AA:BB:CC:DD:EE:FF random --temporary").find(R"("address_type":1,"remember":false)") != std::string::npos,
          "typed BLE selection");
    check(compile("ble unpair all") == R"({"v":1,"id":7,"op":"ble.unpair","all":true})", "explicit all required");
    check(compile("wifi forget 2") == R"({"v":1,"id":7,"op":"wifi.forget","slot":2})", "stable slot IDs");
    check(compile("command 4294967295").find("4294967295") != std::string::npos, "uint32 ticket maximum");
    check(compile(R"( {"v":1,"id":91,"op":"ping"})") == R"({"v":1,"id":91,"op":"ping"})", "machine request IDs preserved");
    for (const auto* command : {"help", "help wifi", "help ble", "help protocol", "status", "ping", "traffic", "capabilities",
        "wifi scan", "wifi results", "wifi results 15", "wifi saved 3", "wifi status", "wifi on", "wifi off",
        "wifi use 0", "wifi disconnect", "wifi reconnect", "wifi remember", "wifi forget all",
        "ble scan", "ble results 2", "ble peer 7", "ble bonds 0", "ble saved", "ble status", "ble on", "ble off",
        "ble use 1", "ble disconnect", "ble reconnect", "ble remember", "ble forget all", "ble unpair AA:BB:CC:DD:EE:FF public"})
        check(!compile(command).empty(), "documented command accepted");
    for (const auto* command : {"", "wifi", "wifi forget", "wifi forget -1", "wifi use all", "wifi use 4", "wifi use 1tail",
        "wifi use 999999999999999999", "wifi scan extra", "wifi results 256", "wifi connect a", "wifi connect a short --typo",
        "wifi connect \"unfinished", "wifi connect a 'password' more extras", "wifi connect a pass\\q", "ble unpair",
        "ble connect XX:BB:CC:DD:EE:FF 0", "ble connect AA:BB:CC:DD:EE:FF 4", "ble connect AA:BB:CC:DD:EE:FF 0.5",
        "command 0", "command 4294967296", "status; wifi off", "a b c d e f g h i"}) rejects(command);
    std::string large(console::kMaxLineBytes + 1, 'a'); rejects(large.c_str());
    std::string json = "{" + std::string(kMaxRequestBytes, ' '); rejects(json.c_str());
    rejects("wifi connect \xff password");
    char tiny[12]{};
    check(console::compile("status", 7, tiny, sizeof(tiny)) != nullptr && !tiny[0], "no truncated JSON dispatched");
}
void test_framing()
{
    using Framer = console::LineFramer;
    using Result = Framer::Result;
    Framer frame;
    for (char c : std::string("wifi scan")) check(frame.push(c) == Result::none, "fragmented command waits for newline");
    check(frame.push('\r') == Result::line && std::string(frame.line()) == "wifi scan", "CR terminates command");
    check(frame.push('\n') == Result::none, "CRLF must not duplicate command");
    frame.push('a'); frame.push('b'); frame.push('\b'); frame.push('c');
    check(frame.push('\n') == Result::line && std::string(frame.line()) == "ac", "backspace");
    for (char c : std::string("中文")) frame.push(c);
    frame.push('\b');
    check(frame.push('\n') == Result::line && std::string(frame.line()) == "中", "backspace removes a whole UTF-8 character");
    frame.erase_line(); check(!frame.line()[0], "consumed credentials can be erased without losing delimiter state");
    for (size_t i = 0; i < console::kMaxLineBytes; ++i) frame.push('x');
    check(frame.push('\n') == Result::line && std::strlen(frame.line()) == console::kMaxLineBytes, "exact maximum");
    for (size_t i = 0; i <= console::kMaxLineBytes; ++i) frame.push('x');
    frame.push('\b');
    for (char c : std::string("wifi off")) frame.push(c);
    check(frame.push('\n') == Result::too_long, "overlong suffix must not execute, even after backspace");
    frame.push('a'); frame.push(0);
    for (char c : std::string("wifi off")) frame.push(c);
    check(frame.push('\n') == Result::invalid_character, "NUL invalidates entire command");
    frame.push('w'); frame.abandon();
    for (char c : std::string("wifi off")) frame.push(c);
    check(frame.push('\n') == Result::invalid_character, "timed-out prefix cannot become another command");
    frame.push(console::kRecordSeparator); frame.push('a');
    check(frame.push('\n') == Result::line && std::string(frame.line()) == "a", "record resynchronization");
    // Deterministic adversarial stream exercises all byte values and boundaries.
    uint32_t seed = 17;
    for (unsigned i = 0; i < 200000; ++i) {
        seed = seed * 1664525U + 1013904223U;
        const auto result = frame.push(static_cast<char>(seed >> 24));
        if (result == Result::line) check(std::strlen(frame.line()) <= console::kMaxLineBytes, "framer invariant");
    }
}
void test_memory()
{
    RadioMemory memory;
    check(memory.remember(wifi("home")), "first Wi-Fi remembered");
    check(memory.remember(wifi("work")), "second Wi-Fi remembered");
    check(memory.preferred_wifi == 1, "last successful target preferred");
    auto changed = wifi("home"); std::strcpy(changed.password, "updated-pass");
    check(memory.remember(changed) && memory.preferred_wifi == 0, "same SSID updates its existing slot");
    memory.forget_wifi(0);
    check(memory.preferred_wifi == kNoSlot && std::string(memory.wifi[1].ssid) == "work", "forget never shifts another slot");
    check(memory.remember(wifi("one")) && memory.remember(wifi("two")) && memory.remember(wifi("three")), "fill all slots");
    const auto before = encode_memory(memory);
    check(!memory.remember(wifi("full")) && encode_memory(memory) == before, "full memory never evicts peers");
    check(memory.remember(BlePeer{"aa:bb:cc:dd:ee:ff", 0}), "public peer persisted");
    check(memory.remember(BlePeer{"AA:BB:CC:DD:EE:FF", 2}) && memory.preferred_ble == 0, "canonical identity is not duplicated");
    check(memory.remember(BlePeer{"CA:BB:CC:DD:EE:FF", 1}), "static random persisted");
    check(!memory.remember(BlePeer{"4A:BB:CC:DD:EE:FF", 1}), "unresolved RPA is not remembered");
    check(!memory.remember(BlePeer{"0A:BB:CC:DD:EE:FF", 1}), "nonresolvable address is not remembered");
    RadioMemory decoded;
    auto bytes = encode_memory(memory);
    check(decode_memory(bytes.data(), bytes.size(), decoded) && encode_memory(decoded) == bytes, "persistence round trip");
    const auto valid = bytes;
    check(!decode_memory(bytes.data(), bytes.size() - 1, decoded), "truncated blob rejected");
    bytes[3] = '2'; check(!decode_memory(bytes.data(), bytes.size(), decoded), "unknown version rejected");
    bytes = valid; bytes[4] = 4; check(!decode_memory(bytes.data(), bytes.size(), decoded), "invalid preferred slot rejected");
    bytes = valid; std::memset(bytes.data() + 6, 'x', 33);
    check(!decode_memory(bytes.data(), bytes.size(), decoded), "unterminated SSID rejected");
    check(encode_memory(decoded) == valid, "failed decode preserves previous valid state");
    memory.forget_wifi(kNoSlot); memory.forget_ble(kNoSlot);
    check(encode_memory(memory) == encode_memory(RadioMemory{}), "forget all wipes credentials and preferences");
}
void test_history()
{
    CommandHistory history;
    const auto ticket = history.next_ticket(); history.queued(ticket, 9);
    const auto other = history.next_ticket(); history.queued(other, 9);
    check(ticket != other && !history.find(ticket).applied, "same client ID has independent tickets");
    history.finish(ticket, 5);
    check(history.find(ticket).applied && history.find(ticket).error == 5 && !history.find(other).applied, "completion isolation");
    for (size_t i = 0; i < CommandHistory::capacity; ++i) { const auto t = history.next_ticket(); history.queued(t, 0); }
    check(!history.find(ticket).ticket, "expired result is explicitly unavailable");
    check(!history.find(0).ticket, "zero is never a valid receipt");
}
}
int main()
{
    test_commands(); test_framing(); test_memory(); test_history();
    std::cout << "Radio console, framing, memory, and command history tests passed\n";
}
