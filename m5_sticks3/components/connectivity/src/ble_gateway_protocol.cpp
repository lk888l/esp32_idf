#include "ble_gateway_protocol.hpp"
#include "ble_transport.hpp"
#include "connectivity_runtime.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

namespace connectivity::gateway {
namespace {
bool integer(const cJSON* value, uint32_t maximum, uint32_t& output)
{
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > maximum ||
        std::floor(value->valuedouble) != value->valuedouble) return false;
    output = static_cast<uint32_t>(value->valuedouble); return true;
}
}
void dispatch(const cJSON* root, uint32_t id, BleTransport& ble, Submit submit,
              void* context, char* output, size_t capacity)
{
    const auto get = [&](const char* key) { return cJSON_GetObjectItemCaseSensitive(root, key); };
    const auto fail = [&](const char* error) {
        std::snprintf(output, capacity, "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}",
                      static_cast<unsigned long>(id), error);
    };
    const std::string_view op(get("op")->valuestring);
    const auto action = op.substr(std::strlen("ble.gatt."));
    const bool status = action == "status", result = action == "result", events = action == "events";
    Request request{};
    bool mutation = !status && !result && !events;
    if (mutation) {
        bool known = false;
        for (auto kind : {Operation::services, Operation::characteristics, Operation::descriptors,
                          Operation::read, Operation::write, Operation::subscribe, Operation::mtu, Operation::pair})
            if (action == operation_name(kind)) { request.operation = kind; known = true; break; }
        if (!known) { fail("unknown_operation"); return; }
    }
    const bool range = action == "characteristics" || action == "descriptors";
    const bool handle = range || action == "read" || action == "write" || action == "subscribe";
    for (auto* field = root->child; field; field = field->next) {
        const std::string_view key(field->string);
        if (key == "v" || key == "id" || key == "op" || key == "token") continue;
        if ((mutation || events) && key == "generation") continue;
        if (handle && key == "handle") continue;
        if (range && key == "end") continue;
        if (action == "write" && key == "data") continue;
        if (action == "subscribe" && key == "mode") continue;
        if (result && (key == "ticket" || key == "index" || key == "offset")) continue;
        if (events && (key == "after" || key == "sequence" || key == "offset")) continue;
        fail("unknown_field"); return;
    }
    uint32_t generation = 0, ticket = 0, index = 0, offset = 0, after = 0, sequence = 0, value = 0;
    if ((mutation || events) && (!integer(get("generation"), UINT32_MAX, generation) || !generation)) {
        fail("invalid_generation"); return;
    }
    if (result && (!integer(get("ticket"), UINT32_MAX, ticket) || !ticket)) { fail("invalid_ticket"); return; }
    if (get("index") && !integer(get("index"), kRows - 1, index)) { fail("invalid_index"); return; }
    if (get("offset") && !integer(get("offset"), kValueBytes, offset)) { fail("invalid_offset"); return; }
    if (get("after") && !integer(get("after"), UINT32_MAX, after)) { fail("invalid_cursor"); return; }
    if (get("sequence") && (!integer(get("sequence"), UINT32_MAX, sequence) || !sequence)) { fail("invalid_cursor"); return; }
    if (events && ((get("after") && get("sequence")) || (offset && !sequence))) { fail("invalid_cursor"); return; }
    if (mutation) {
        request.generation = generation;
        if (handle) {
            if (!integer(get("handle"), UINT16_MAX, value) || !value) { fail("invalid_handle"); return; }
            request.handle = value;
        }
        if (range) {
            if (!integer(get("end"), UINT16_MAX, value) || !value) { fail("invalid_range"); return; }
            request.end = value;
        }
        if (action == "subscribe") {
            if (!integer(get("mode"), 2, value)) { fail("invalid_mode"); return; }
            request.mode = value;
        }
        if (action == "write") {
            const auto* data = get("data");
            if (!cJSON_IsString(data) || !decode_hex(data->valuestring, request.data, sizeof(request.data), request.length)) {
                fail("invalid_data"); return;
            }
        }
        if (!valid(request)) { fail("invalid_request"); return; }
        const auto current = ble.gatt_status();
        if (!current.connected) { fail("disconnected"); return; }
        if (generation != current.generation) { fail("stale_generation"); return; }
        if (current.active_ticket) { fail("busy"); return; }
        const auto error = submit(context, request, id, &ticket);
        if (error != ESP_OK) { fail(error == ESP_ERR_NOT_SUPPORTED ? "disabled" : "busy"); return; }
        std::snprintf(output, capacity, "{\"v\":1,\"id\":%lu,\"ok\":true,\"result\":\"accepted\",\"ticket\":%lu}",
                      static_cast<unsigned long>(id), static_cast<unsigned long>(ticket));
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> reply(cJSON_CreateObject(), cJSON_Delete);
    if (!reply) { fail("no_memory"); return; }
    bool built = cJSON_AddNumberToObject(reply.get(), "v", 1) &&
        cJSON_AddNumberToObject(reply.get(), "id", id) && cJSON_AddBoolToObject(reply.get(), "ok", true);
    const auto number = [&](const char* key, double number) { built = cJSON_AddNumberToObject(reply.get(), key, number) && built; };
    const auto string = [&](const char* key, const char* text) { built = cJSON_AddStringToObject(reply.get(), key, text) && built; };
    const auto data = [&](const uint8_t* bytes, size_t length) {
        char hex[kPageBytes * 2 + 1]{}; encode_hex(bytes, length, hex); string("data", hex);
    };
    if (status) {
        const auto current = ble.gatt_status();
        number("connected", current.connected); number("generation", current.generation);
        number("active_ticket", current.active_ticket); number("latest_event", current.latest_event);
        number("dropped_events", current.dropped_events); number("write_bytes", kWriteBytes);
        number("value_bytes", kValueBytes); number("page_bytes", kPageBytes);
        number("result_capacity", kResults); number("event_capacity", kEvents); number("row_capacity", kRows);
        number("mtu", ble.diagnostics().peer_mtu);
    } else if (result) {
        const auto page = ble.gatt_result(ticket, index, offset);
        if (!page.ticket) {
            const auto command = command_result(ticket);
            if (!command.ticket) { fail("unknown_ticket"); return; }
            if (command.applied && !command.error) { fail("result_expired"); return; }
            number("ticket", ticket); string("state", command.applied ? "failed" : "queued");
            number("error_code", command.error); string("error_domain", "esp");
        } else {
            if (page.state == State::complete && ((page.count && index >= page.count) || offset > page.length)) {
                fail(offset > page.length ? "invalid_offset" : "invalid_index"); return;
            }
            number("ticket", ticket); number("generation", page.generation);
            string("operation", operation_name(page.operation));
            string("state", page.state == State::queued ? "queued" : page.state == State::running ? "running" : page.error ? "failed" : "complete");
            number("error_code", page.error); string("error_name", error_name(page.error));
            number("count", page.count); number("total", page.total); number("truncated", page.truncated);
            if (page.state == State::complete && page.count) {
                number("index", index); string("uuid", page.row.uuid); number("handle", page.row.handle);
                number("end", page.row.end); number("value_handle", page.row.value_handle); number("properties", page.row.properties);
            } else {
                number("handle", page.handle); number("length", page.length); number("offset", offset); data(page.data, page.bytes);
            }
        }
    } else {
        const auto current = ble.gatt_status();
        if (current.generation != generation) { fail("stale_generation"); return; }
        if (!current.connected) { fail("disconnected"); return; }
        if (after > current.latest_event) { fail("invalid_cursor"); return; }
        const auto page = ble.gatt_event(after, sequence, offset);
        if (page.sequence && page.generation != generation) { fail("stale_generation"); return; }
        if (sequence && !page.sequence) { fail("event_expired"); return; }
        if (offset > page.length) { fail("invalid_offset"); return; }
        number("found", page.sequence != 0); number("sequence", page.sequence); number("generation", generation);
        number("lost", page.lost); number("handle", page.handle); number("indication", page.indication);
        number("length", page.length); number("offset", offset); number("truncated", page.truncated); data(page.data, page.bytes);
    }
    if (!built) { fail("no_memory"); return; }
    if (!cJSON_PrintPreallocated(reply.get(), output, capacity, false)) fail("response_too_large");
}
} // namespace connectivity::gateway
