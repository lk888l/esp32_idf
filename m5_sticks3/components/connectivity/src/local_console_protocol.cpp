#include "local_console_protocol.hpp"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace connectivity::console {
namespace {
struct Tokens {
    std::array<char, kMaxLineBytes + 1> text{};
    std::array<const char*, 8> values{};
    size_t count = 0;
    const char* parse(const char* input)
    {
        size_t out = 0;
        while (*input) {
            while (*input == ' ' || *input == '\t') ++input;
            if (!*input) break;
            if (count == values.size()) return "too_many_arguments";
            values[count++] = text.data() + out;
            char quote = 0;
            while (*input && (quote || (*input != ' ' && *input != '\t'))) {
                char c = *input++;
                if (c == '\\') {
                    if (!*input) return "invalid_escape";
                    c = *input++;
                    if (c != '\\' && c != '"' && c != '\'' && c != ' ' && c != '\t') return "invalid_escape";
                } else if (c == '"' || c == '\'') {
                    if (!quote) { quote = c; continue; }
                    if (quote == c) { quote = 0; continue; }
                }
                if (out + 1 >= text.size()) return "line_too_long";
                text[out++] = c;
            }
            if (quote) return "unterminated_quote";
            text[out++] = 0;
        }
        return nullptr;
    }
    std::string_view at(size_t index) const { return index < count ? values[index] : ""; }
};
struct Writer {
    char* text;
    size_t capacity, used = 0;
    bool valid = true;
    void append(std::string_view value)
    {
        if (used + value.size() >= capacity) { valid = false; return; }
        std::memcpy(text + used, value.data(), value.size()); used += value.size(); text[used] = 0;
    }
    void quote(std::string_view value)
    {
        append("\"");
        for (const char c : value) {
            if (c == '\\' || c == '"') append("\\");
            if (static_cast<unsigned char>(c) < 32) { valid = false; return; }
            append({&c, 1});
        }
        append("\"");
    }
    void string(const char* key, std::string_view value) { append(",\""); append(key); append("\":"); quote(value); }
    void number(const char* key, uint32_t value)
    {
        char digits[11]{}; std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
        append(",\""); append(key); append("\":"); append(digits);
    }
};
bool unsigned_number(std::string_view input, uint32_t& value)
{
    if (input.empty()) return false;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}
bool address_type(std::string_view input, uint32_t& value)
{
    if (input == "public") value = 0;
    else if (input == "random") value = 1;
    else if (input == "public-id") value = 2;
    else if (input == "random-id") value = 3;
    else if (!unsigned_number(input, value) || value > 3) return false;
    return true;
}
} // namespace

const char* compile(const char* line, uint32_t id, char* json, size_t capacity)
{
    if (!line || !json || capacity < 2) return "invalid_argument";
    json[0] = 0;
    const size_t length = strnlen(line, kMaxLineBytes + 1);
    if (length > kMaxLineBytes) return "line_too_long";
    if (!valid_utf8({line, length})) return "invalid_utf8";
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '{') {
        const size_t bytes = std::strlen(line);
        if (bytes > kMaxRequestBytes || bytes >= capacity) return "request_too_large";
        std::memcpy(json, line, bytes + 1); return nullptr;
    }
    Tokens args;
    if (const char* error = args.parse(line)) return error;
    if (!args.count) return "empty_command";
    Writer out{json, capacity};
    out.append("{\"v\":1"); out.number("id", id);
    auto op = [&](std::string_view name) { out.string("op", name); };
    const auto first = args.at(0);
    if (first == "help") {
        if (args.count > 2) return "usage";
        op("help"); if (args.count == 2) out.string("topic", args.at(1));
    } else if (first == "status" || first == "ping" || first == "traffic" || first == "capabilities") {
        if (args.count != 1) return "usage";
        op(first);
    } else if (first == "command") {
        uint32_t ticket;
        if (args.count != 2 || !unsigned_number(args.at(1), ticket) || ticket == 0) return "invalid_ticket";
        op("command.result"); out.number("ticket", ticket);
    } else if (first == "gatt") {
        const auto action = args.at(1);
        char operation[40]{};
        std::snprintf(operation, sizeof(operation), "ble.gatt.%.*s", static_cast<int>(action.size()), action.data());
        op(operation);
        const auto numeric = [&](size_t index, const char* key, uint32_t maximum, bool nonzero) {
            uint32_t value = 0;
            if (!unsigned_number(args.at(index), value) || value > maximum || (nonzero && !value)) return false;
            out.number(key, value); return true;
        };
        if (action == "status") {
            if (args.count != 2) return "usage";
        } else if (action == "result") {
            if (args.count < 3 || args.count > 5) return "usage";
            if (!numeric(2, "ticket", UINT32_MAX, true)) return "invalid_ticket";
            if (args.count >= 4 && !numeric(3, "index", 15, false)) return "invalid_index";
            if (args.count == 5 && !numeric(4, "offset", 512, false)) return "invalid_offset";
        } else {
            const bool range = action == "characteristics" || action == "descriptors";
            const bool handle = range || action == "read" || action == "write" || action == "subscribe";
            const bool simple = action == "services" || action == "mtu" || action == "pair";
            const size_t expected = range || action == "write" || action == "subscribe" ? 5 : handle ? 4 : 3;
            if (!simple && !handle && action != "events") return "unknown_command";
            if (args.count != expected && !(action == "events" && args.count == 4)) return "usage";
            if (!numeric(2, "generation", UINT32_MAX, true)) return "invalid_generation";
            if (handle && !numeric(3, "handle", UINT16_MAX, true)) return "invalid_handle";
            if (range && !numeric(4, "end", UINT16_MAX, true)) return "invalid_range";
            if (action == "subscribe" && !numeric(4, "mode", 2, false)) return "invalid_mode";
            if (action == "write") out.string("data", args.at(4));
            if (action == "events" && args.count == 4 && !numeric(3, "after", UINT32_MAX, false)) return "invalid_cursor";
        }
    } else if (first == "wifi" || first == "ble") {
        const bool wifi = first == "wifi";
        const auto action = args.at(1);
        char operation[32]{};
        const auto radio_op = [&](std::string_view suffix) {
            std::snprintf(operation, sizeof(operation), "%s.%.*s", wifi ? "wifi" : "ble",
                          static_cast<int>(suffix.size()), suffix.data()); op(operation);
        };
        if (action == "connect") {
            if (args.count != 4 && args.count != 5) return "usage";
            const auto option = args.at(4);
            if (args.count == 5 && option != "--remember" && option != "--temporary") return "invalid_option";
            radio_op("connect");
            if (wifi) { out.string("ssid", args.at(2)); out.string("password", args.at(3)); }
            else {
                uint32_t type;
                if (!valid_ble_address(args.at(2)) || !address_type(args.at(3), type)) return "invalid_peer";
                out.string("address", args.at(2)); out.number("address_type", type);
            }
            out.append(option == "--temporary" ? ",\"remember\":false" : ",\"remember\":true");
        } else if (action == "results" || action == "saved" || (!wifi && (action == "peer" || action == "bonds"))) {
            if (args.count != 2 && args.count != 3) return "usage";
            radio_op(action == "results" ? "scan.results" : action);
            if (args.count == 3) {
                uint32_t index;
                if (!unsigned_number(args.at(2), index) || index > 255) return "invalid_index";
                out.number("index", index);
            }
        } else if (action == "forget" || action == "use") {
            if (args.count != 3) return "usage";
            radio_op(action);
            if (action == "forget" && args.at(2) == "all") out.append(",\"all\":true");
            else {
                uint32_t slot;
                if (!unsigned_number(args.at(2), slot) || slot >= 4) return "invalid_slot";
                out.number("slot", slot);
            }
        } else if (!wifi && action == "unpair") {
            radio_op("unpair");
            if (args.count == 3 && args.at(2) == "all") out.append(",\"all\":true");
            else {
                uint32_t type;
                if (args.count != 4 || !valid_ble_address(args.at(2)) || !address_type(args.at(3), type)) return "invalid_peer";
                out.string("address", args.at(2)); out.number("address_type", type);
            }
        } else if (action == "status" || action == "scan" || action == "disconnect" ||
                   action == "reconnect" || action == "remember" || action == "on" || action == "off") {
            if (args.count != 2) return "usage";
            radio_op(action == "on" ? "enable" : action == "off" ? "disable" : action);
        } else return "unknown_command";
    } else return "unknown_command";
    out.append("}");
    if (!out.valid || out.used > kMaxRequestBytes) { json[0] = 0; return "request_too_large"; }
    return nullptr;
}
} // namespace connectivity::console
