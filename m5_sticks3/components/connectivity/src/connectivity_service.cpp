#include "connectivity_service.hpp"
#include "connectivity_runtime.hpp"
#include "connectivity_policy.hpp"
#include "wifi_transport.hpp"
#include "ble_transport.hpp"
#include "serial_console.hpp"
#include "ble_gateway_protocol.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "motion_state.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "bootloader_random.h"

namespace connectivity {
namespace {
constexpr char kTag[] = "connectivity";
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot shared_state{};
WifiDiagnostics shared_wifi{};
BleDiagnostics shared_ble{};
TrafficSnapshot shared_traffic{};
MemorySnapshot shared_memory{};
portMUX_TYPE history_lock = portMUX_INITIALIZER_UNLOCKED;
CommandHistory history{};
enum class Origin { http, ble, serial };
char local_ap_password[17]{};
using LocalSubmit = esp_err_t (*)(void*, const ControlRequest&);
LocalSubmit local_submit = nullptr;
void* local_context = nullptr;
void publish(const Snapshot& value)
{
    portENTER_CRITICAL(&state_lock);
    shared_state = value;
    portEXIT_CRITICAL(&state_lock);
}
struct Settings {
    uint32_t version = 1;
    WifiCredentials wifi{};
    char token[33]{};
    char ap_password[17]{};
};
struct Command {
    ControlRequest control{};
    uint32_t id = 0;
    uint32_t ticket = 0;
};
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

void random_hex(char* output, size_t bytes)
{
    std::array<uint8_t, 16> random{};
    esp_fill_random(random.data(), bytes);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        output[i * 2] = digits[random[i] >> 4];
        output[i * 2 + 1] = digits[random[i] & 15];
    }
    output[bytes * 2] = '\0';
}
bool hex_secret(const char* value, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return value[size] == '\0';
}
void error_response(char* output, size_t capacity, uint32_t id, const char* error)
{
    snprintf(output, capacity, "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}",
             static_cast<unsigned long>(id), error);
}
bool copy_string(const cJSON* value, char* output, size_t capacity)
{
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    const size_t length = strlen(value->valuestring);
    if (length >= capacity) return false;
    memcpy(output, value->valuestring, length + 1);
    return true;
}
bool unsigned_json(const cJSON* value, uint32_t maximum, uint32_t& output)
{
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > maximum ||
        std::floor(value->valuedouble) != value->valuedouble) return false;
    output = static_cast<uint32_t>(value->valuedouble);
    return true;
}
// Reject deep nesting before cJSON recursion, embedded NULs, and escaped NULs
// (cJSON strings cannot represent them without truncation).
bool bounded_json(const char* text, size_t length)
{
    if (!text || !length || length > kMaxRequestBytes || memchr(text, 0, length)) return false;
    if (!valid_utf8({text, length})) return false;
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (quoted && c == '\\') {
            if (i + 5 < length && memcmp(text + i, "\\u0000", 6) == 0) return false;
            ++i;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == '{' || c == '[') && ++depth > 4) return false;
        if (!quoted && (c == '}' || c == ']')) {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}
bool unique_keys(const cJSON* object)
{
    for (auto* a = object->child; a; a = a->next) {
        for (auto* b = a->next; b; b = b->next) {
            if (a->string && b->string && strcmp(a->string, b->string) == 0) return false;
        }
    }
    return true;
}
} // namespace

Snapshot snapshot()
{
    portENTER_CRITICAL(&state_lock);
    const Snapshot value = shared_state;
    portEXIT_CRITICAL(&state_lock);
    return value;
}
WifiDiagnostics wifi_diagnostics()
{
    portENTER_CRITICAL(&state_lock);
    const auto value = shared_wifi;
    portEXIT_CRITICAL(&state_lock);
    return value;
}
BleDiagnostics ble_diagnostics()
{
    portENTER_CRITICAL(&state_lock);
    const auto value = shared_ble;
    portEXIT_CRITICAL(&state_lock);
    return value;
}
TrafficSnapshot traffic_snapshot()
{
    portENTER_CRITICAL(&state_lock);
    const auto value = shared_traffic;
    portEXIT_CRITICAL(&state_lock);
    return value;
}
MemorySnapshot memory_snapshot()
{
    portENTER_CRITICAL(&state_lock);
    const auto value = shared_memory;
    portEXIT_CRITICAL(&state_lock);
    return value;
}
CommandResult command_result(uint32_t ticket)
{
    portENTER_CRITICAL(&history_lock);
    const auto value = history.find(ticket);
    portEXIT_CRITICAL(&history_lock);
    return value;
}
esp_err_t request_control(const ControlRequest& request)
{
    // The bridge and its owner are detached under this same lock before teardown.
    // Submission only copies to a zero-wait fixed queue, never invokes a driver.
    portENTER_CRITICAL(&state_lock);
    const esp_err_t result = local_submit ? local_submit(local_context, request) : ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&state_lock);
    return result;
}
bool copy_local_ap_password(char* output, size_t capacity)
{
    if (!output || capacity < sizeof(local_ap_password)) return false;
    portENTER_CRITICAL(&state_lock);
    const bool available = local_submit && local_ap_password[0];
    if (available) memcpy(output, local_ap_password, sizeof(local_ap_password));
    else output[0] = '\0';
    portEXIT_CRITICAL(&state_lock);
    return available;
}
const char* wifi_state_name(WifiState state)
{
    switch (state) {
    case WifiState::stopped: return "off";
    case WifiState::access_point: return "ap";
    case WifiState::connecting: return "connecting";
    case WifiState::connected: return "connected";
    case WifiState::retry_wait: return "retry";
    case WifiState::failed: return "failed";
    }
    return "unknown";
}

struct Service::Impl {
    WifiTransport wifi;
    BleTransport ble;
    SerialConsole console;
    Settings settings{};
    RadioMemory memory{};
    WifiCredentials current_wifi{};
    BlePeer pending_ble{};
    uint32_t ble_generation = 0;
    int64_t wifi_remember_deadline = 0, ble_remember_deadline = 0;
    int32_t wifi_memory_error = 0, ble_memory_error = 0;
    nvs_handle_t storage = 0;
    bool nvs_ready = false;
    bool event_loop_owned = false;
    bool initialized = false;
    bool wifi_started = false;
    bool ble_started = false;
    bool wifi_attempted = false;
    bool ble_attempted = false;
    std::atomic<bool> accepting{false};
    std::atomic<uint32_t> accepted{0};
    std::atomic<uint32_t> rejected{0};
    Snapshot state{};
    static constexpr size_t kCommandCapacity = 4;
    alignas(Command) std::array<uint8_t, sizeof(Command) * kCommandCapacity> queue_storage{};
    StaticQueue_t queue_state{};
    QueueHandle_t queue = nullptr;
    uint32_t local_sequence = 0x40000000;

    Impl()
    {
        queue = xQueueCreateStatic(kCommandCapacity, sizeof(Command),
                                   queue_storage.data(), &queue_state);
    }

    static esp_err_t submit_local(void* context, const ControlRequest& request)
    {
        auto& self = *static_cast<Impl*>(context);
        return self.enqueue(request, ++self.local_sequence);
    }

    esp_err_t enqueue(const ControlRequest& request, uint32_t id, uint32_t* ticket = nullptr)
    {
        if (!accepting.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
        switch (request.action) {
        case ControlAction::ble_gatt:
            if (!gateway::valid(request.gatt)) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::wifi_connect: case ControlAction::wifi_configure:
            if (!valid_credentials(request.wifi)) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::ble_connect:
            if (strnlen(request.address, sizeof(request.address)) != 17 ||
                !valid_ble_address(request.address) || request.address_type > 3) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::ble_unpair:
            if (!request.all && (strnlen(request.address, sizeof(request.address)) != 17 ||
                !valid_ble_address(request.address) || request.address_type > 3)) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::wifi_use: case ControlAction::ble_use:
            if (request.slot >= kMemorySlots) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::wifi_forget: case ControlAction::ble_forget:
            if (!request.all && request.slot >= kMemorySlots) return ESP_ERR_INVALID_ARG;
            break;
        case ControlAction::wifi_scan: case ControlAction::wifi_reconnect:
        case ControlAction::wifi_disconnect: case ControlAction::wifi_enable:
        case ControlAction::wifi_disable: case ControlAction::wifi_clear:
        case ControlAction::ble_scan: case ControlAction::ble_disconnect:
        case ControlAction::ble_enable: case ControlAction::ble_disable:
        case ControlAction::wifi_remember: case ControlAction::ble_remember:
        case ControlAction::ble_reconnect:
            break;
        default: return ESP_ERR_INVALID_ARG;
        }
        if (request.action <= ControlAction::wifi_use) {
#if !CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
            return ESP_ERR_NOT_SUPPORTED;
#endif
        } else {
#if !CONFIG_M5_CONNECTIVITY_BLE_ENABLED
            return ESP_ERR_NOT_SUPPORTED;
#endif
        }
        if (!queue) return ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&history_lock);
        if (uxQueueSpacesAvailable(queue) == 0) {
            portEXIT_CRITICAL(&history_lock);
            return ESP_ERR_TIMEOUT;
        }
        Command command{request, id, history.next_ticket()};
        const bool sent = xQueueSend(queue, &command, 0) == pdPASS;
        if (sent) history.queued(command.ticket, id);
        portEXIT_CRITICAL(&history_lock);
        if (!sent) return ESP_ERR_TIMEOUT;
        if (ticket) *ticket = command.ticket;
        accepted.fetch_add(1, std::memory_order_relaxed);
        return ESP_OK;
    }

    esp_err_t save_memory(const RadioMemory& updated)
    {
        const auto encoded = encode_memory(updated);
        if (encoded == encode_memory(memory)) return ESP_OK;
        esp_err_t result = nvs_set_blob(storage, "radio_mem", encoded.data(), encoded.size());
        if (result == ESP_OK) result = nvs_commit(storage);
        if (result == ESP_OK) memory = updated;
        return result;
    }

    esp_err_t connect_wifi(const WifiCredentials& credentials, bool remember, bool legacy = false)
    {
        const esp_err_t ready = wifi.configuration_ready();
        if (ready != ESP_OK) return ready;
        if (remember && memory.wifi_slot(credentials.ssid) == kNoSlot) return ESP_ERR_NO_MEM;
        if (legacy) {
            RadioMemory updated = memory;
            if (!updated.remember(credentials)) return ESP_ERR_NO_MEM;
            const auto saved = save_memory(updated);
            if (saved != ESP_OK) return saved;
        }
        esp_err_t result = ESP_OK;
        if (!wifi.diagnostics().enabled) result = wifi.set_enabled(true);
        if (result == ESP_OK) result = wifi.configure(credentials);
        // configure invalidates the previous association before applying the
        // new one. Failed replacement must not later persist an older intent.
        wifi_remember_deadline = 0;
        if (result == ESP_OK) {
            current_wifi = credentials;
            wifi_memory_error = 0;
            if (remember && !legacy) wifi_remember_deadline = esp_timer_get_time() + 120'000'000;
        }
        return result;
    }

    esp_err_t connect_ble(BlePeer peer, bool remember, bool saved)
    {
        if (!normalize_peer(peer)) return ESP_ERR_INVALID_ARG;
        const auto data = ble.diagnostics();
        const auto result = ble.connect_peer(peer.address, peer.address_type, saved);
        if (result == ESP_OK) {
            pending_ble = peer;
            ble_generation = data.peer_generation;
            ble_memory_error = 0;
            ble_remember_deadline = remember ? esp_timer_get_time() + 30'000'000 : 0;
        }
        return result;
    }

    esp_err_t remember_wifi()
    {
        const auto data = wifi.snapshot();
        if (data.state != WifiState::connected || !data.address[0] ||
            std::strcmp(data.ssid, current_wifi.ssid) != 0 ||
            !wifi.connection_matches(current_wifi)) return ESP_ERR_INVALID_STATE;
        RadioMemory updated = memory;
        return updated.remember(current_wifi) ? save_memory(updated) : ESP_ERR_NO_MEM;
    }
    esp_err_t remember_ble()
    {
        const auto data = ble.diagnostics();
        if (!data.peer_connected || data.switching) return ESP_ERR_INVALID_STATE;
        BlePeer peer{};
        std::memcpy(peer.address, data.peer_identity, sizeof(peer.address));
        peer.address_type = data.peer_identity_type;
        if (!stable_peer(peer)) return ESP_ERR_NOT_SUPPORTED;
        RadioMemory updated = memory;
        return updated.remember(peer) ? save_memory(updated) : ESP_ERR_NO_MEM;
    }

    void process_memory()
    {
        const auto now = esp_timer_get_time();
        if (wifi_remember_deadline) {
            const auto data = wifi.snapshot();
            if (data.state == WifiState::connected && data.address[0] &&
                std::strcmp(data.ssid, current_wifi.ssid) == 0) {
                const auto result = remember_wifi();
                if (result != ESP_ERR_INVALID_STATE) { wifi_memory_error = result; wifi_remember_deadline = 0; }
            }
            if (wifi_remember_deadline && now >= wifi_remember_deadline) {
                wifi_memory_error = ESP_ERR_TIMEOUT; wifi_remember_deadline = 0;
            }
        }
        if (ble_remember_deadline) {
            const auto data = ble.diagnostics();
            BlePeer connected{};
            std::memcpy(connected.address, data.peer_address, sizeof(connected.address));
            connected.address_type = data.peer_address_type;
            if (data.peer_connected && !data.switching && data.peer_generation != ble_generation &&
                same_peer(connected, pending_ble)) {
                ble_memory_error = remember_ble(); ble_remember_deadline = 0;
            } else if (now >= ble_remember_deadline) {
                ble_memory_error = ESP_ERR_TIMEOUT; ble_remember_deadline = 0;
            }
        }
    }

    esp_err_t execute(const ControlRequest& request, uint32_t ticket)
    {
        switch (request.action) {
        case ControlAction::ble_gatt: return ble.request_gatt(request.gatt, ticket);
        case ControlAction::wifi_scan: return wifi.request_scan();
        case ControlAction::wifi_enable: return wifi.set_enabled(true);
        case ControlAction::wifi_disable: case ControlAction::wifi_disconnect: {
            const auto result = request.action == ControlAction::wifi_disable
                ? wifi.set_enabled(false) : wifi.disconnect_station();
            if (result == ESP_OK) wifi_remember_deadline = 0;
            return result;
        }
        case ControlAction::wifi_connect: return connect_wifi(request.wifi, request.remember);
        case ControlAction::wifi_configure: return connect_wifi(request.wifi, true, true);
        case ControlAction::wifi_reconnect: case ControlAction::wifi_use: {
            const auto slot = request.action == ControlAction::wifi_use ? request.slot : memory.preferred_wifi;
            if (slot >= kMemorySlots || !memory.wifi[slot].ssid[0]) return ESP_ERR_NOT_FOUND;
            return connect_wifi(memory.wifi[slot], true);
        }
        case ControlAction::wifi_remember:
            wifi_memory_error = remember_wifi(); wifi_remember_deadline = 0; return wifi_memory_error;
        case ControlAction::wifi_forget: case ControlAction::wifi_clear: {
            const bool clear = request.action == ControlAction::wifi_clear;
            if (clear) { const auto ready = wifi.configuration_ready(); if (ready != ESP_OK) return ready; }
            const auto slot = clear || request.all ? kNoSlot : request.slot;
            if (slot != kNoSlot && !memory.wifi[slot].ssid[0]) return ESP_ERR_NOT_FOUND;
            const bool cancel = slot == kNoSlot || std::strcmp(memory.wifi[slot].ssid, current_wifi.ssid) == 0;
            RadioMemory updated = memory; updated.forget_wifi(slot);
            const auto result = save_memory(updated);
            if (result != ESP_OK) return result;
            if (cancel) { wifi_remember_deadline = 0; wifi_memory_error = 0; }
            if (clear) { current_wifi = {}; return wifi.configure({}); }
            return ESP_OK;
        }
        case ControlAction::ble_scan: return ble.request_scan();
        case ControlAction::ble_enable: return ble.set_enabled(true);
        case ControlAction::ble_disable: case ControlAction::ble_disconnect: {
            const auto result = request.action == ControlAction::ble_disable
                ? ble.set_enabled(false) : ble.disconnect_peer();
            if (result == ESP_OK) ble_remember_deadline = 0;
            return result;
        }
        case ControlAction::ble_connect: {
            BlePeer peer{}; std::memcpy(peer.address, request.address, sizeof(peer.address));
            peer.address_type = request.address_type;
            return connect_ble(peer, request.remember, false);
        }
        case ControlAction::ble_use: case ControlAction::ble_reconnect: {
            const auto slot = request.action == ControlAction::ble_use ? request.slot : memory.preferred_ble;
            if (slot >= kMemorySlots || !memory.ble[slot].address[0]) return ESP_ERR_NOT_FOUND;
            return connect_ble(memory.ble[slot], true, true);
        }
        case ControlAction::ble_remember:
            ble_memory_error = remember_ble(); ble_remember_deadline = 0; return ble_memory_error;
        case ControlAction::ble_forget: {
            const auto slot = request.all ? kNoSlot : request.slot;
            if (slot != kNoSlot && !memory.ble[slot].address[0]) return ESP_ERR_NOT_FOUND;
            RadioMemory updated = memory; updated.forget_ble(slot);
            const auto result = save_memory(updated);
            // Cancel any in-flight save so a later callback cannot undo forget.
            if (result == ESP_OK) { ble_remember_deadline = 0; ble_memory_error = 0; }
            return result;
        }
        case ControlAction::ble_unpair:
            ble_remember_deadline = 0;
            return ble.unpair(request.address, request.address_type, request.all);
        }
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t persist(const Settings& value)
    {
        esp_err_t result = nvs_set_blob(storage, "settings", &value, sizeof(value));
        if (result == ESP_OK) result = nvs_commit(storage);
        return result;
    }

    esp_err_t load()
    {
        esp_err_t result = nvs_open("stick_net", NVS_READWRITE, &storage);
        if (result != ESP_OK) return result;
        size_t size = sizeof(settings);
        result = nvs_get_blob(storage, "settings", &settings, &size);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            settings = {};
            // Enable the hardware entropy source before any RF driver starts.
            bootloader_random_enable();
            random_hex(settings.token, 16);
            random_hex(settings.ap_password, 8);
            bootloader_random_disable();
            result = persist(settings);
        }
        if (result != ESP_OK) return result;
        if (size != sizeof(settings) || settings.version != 1 ||
            !hex_secret(settings.token, 32) || !hex_secret(settings.ap_password, 16) ||
            (settings.wifi.ssid[0] && !valid_credentials(settings.wifi))) {
            return ESP_ERR_INVALID_VERSION;
        }
        MemoryBytes bytes{};
        size = bytes.size();
        result = nvs_get_blob(storage, "radio_mem", bytes.data(), &size);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            memory = {};
            if (settings.wifi.ssid[0]) memory.remember(settings.wifi);
            bytes = encode_memory(memory);
            result = nvs_set_blob(storage, "radio_mem", bytes.data(), bytes.size());
            if (result == ESP_OK) result = nvs_commit(storage);
        } else if (result == ESP_OK && !decode_memory(bytes.data(), size, memory)) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (result != ESP_OK) return result;
        // Commit the new record first, then retire legacy credentials. A reset
        // between commits repeats cleanup, never resurrects a forgotten peer.
        if (settings.wifi.ssid[0] || settings.wifi.password[0]) {
            Settings cleaned = settings; cleaned.wifi = {};
            result = persist(cleaned);
            if (result != ESP_OK) return result;
            settings = cleaned;
        }
        current_wifi = memory.preferred_wifi < kMemorySlots
            ? memory.wifi[memory.preferred_wifi] : WifiCredentials{};
        return ESP_OK;
    }

    void update()
    {
        state.wifi = wifi.snapshot();
        state.ble = ble.snapshot();
        state.ready = initialized;
        state.accepted = accepted.load(std::memory_order_relaxed);
        state.rejected = rejected.load(std::memory_order_relaxed);
        publish(state);
        // Transport snapshots never nest their locks with this publication lock.
        const auto wifi_data = wifi.diagnostics();
        const auto ble_data = ble.diagnostics();
        MemorySnapshot remembered{};
        for (size_t i = 0; i < kMemorySlots; ++i) {
            memcpy(remembered.wifi_ssids[i], memory.wifi[i].ssid, sizeof(remembered.wifi_ssids[i]));
            remembered.ble[i] = memory.ble[i];
        }
        remembered.preferred_wifi = memory.preferred_wifi;
        remembered.preferred_ble = memory.preferred_ble;
        remembered.wifi_pending = wifi_remember_deadline != 0;
        remembered.ble_pending = ble_remember_deadline != 0;
        remembered.wifi_error = wifi_memory_error;
        remembered.ble_error = ble_memory_error;
        portENTER_CRITICAL(&state_lock);
        shared_wifi = wifi_data;
        shared_ble = ble_data;
        shared_memory = remembered;
        portEXIT_CRITICAL(&state_lock);
    }

    static void handle_wifi(void* context, const char* input, size_t length,
                            char* output, size_t capacity)
    {
        static_cast<Impl*>(context)->request(input, length, output, capacity, Origin::http);
    }
    static void handle_ble(void* context, const char* input, size_t length,
                           char* output, size_t capacity)
    {
        static_cast<Impl*>(context)->request(input, length, output, capacity, Origin::ble);
    }
    static void handle_serial(void* context, const char* input, size_t length,
                              char* output, size_t capacity)
    {
        static_cast<Impl*>(context)->request(input, length, output, capacity, Origin::serial);
    }

    void request(const char* input, size_t length, char* output, size_t capacity, Origin origin)
    {
        struct Record {
            Origin origin;
            size_t length;
            char* output;
            size_t capacity;
            uint32_t id = 0;
            bool success = true;
            char operation[24] = "invalid";
            char echo[65]{};
            bool has_echo = false;
            ~Record()
            {
                portENTER_CRITICAL(&state_lock);
                auto& value = shared_traffic;
                if (origin == Origin::ble) ++value.ble_requests;
                else if (origin == Origin::serial) ++value.serial_requests;
                else ++value.http_requests;
                value.rx_bytes += length;
                value.tx_bytes += output ? strnlen(output, capacity) : 0;
                strcpy(value.last_transport, origin == Origin::ble ? "BLE" : origin == Origin::serial ? "USB" : "HTTP");
                memcpy(value.last_operation, operation, sizeof(operation));
                value.last_request_id = id;
                value.last_ok = success;
                if (has_echo) memcpy(value.echo, echo, sizeof(echo));
                portEXIT_CRITICAL(&state_lock);
            }
        } record{origin, length, output, capacity};
        uint32_t id = 0;
        const auto fail = [&](const char* why) {
            record.success = false;
            rejected.fetch_add(1, std::memory_order_relaxed);
            error_response(output, capacity, id, why);
        };
        if (!output || capacity < 96) { record.success = false; return; }
        output[0] = '\0';
        if (!accepting.load(std::memory_order_acquire)) { fail("unavailable"); return; }
        if (!bounded_json(input, length)) { fail("invalid_json"); return; }
        std::array<char, kMaxRequestBytes + 1> buffer{};
        memcpy(buffer.data(), input, length);
        const char* end = nullptr;
        Json root(cJSON_ParseWithLengthOpts(buffer.data(), length + 1, &end, true), cJSON_Delete);
        if (!root || !cJSON_IsObject(root.get()) || !unique_keys(root.get())) {
            fail("invalid_json"); return;
        }
        auto get = [&](const char* key) { return cJSON_GetObjectItemCaseSensitive(root.get(), key); };
        const cJSON* version = get("v");
        const cJSON* request_id = get("id");
        const cJSON* operation = get("op");
        if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
            !cJSON_IsNumber(request_id) || !std::isfinite(request_id->valuedouble) ||
            request_id->valuedouble < 0 || request_id->valuedouble > 2147483647.0 ||
            std::floor(request_id->valuedouble) != request_id->valuedouble ||
            !cJSON_IsString(operation)) {
            fail("invalid_envelope"); return;
        }
        id = static_cast<uint32_t>(request_id->valuedouble);
        const std::string_view op(operation->valuestring);
        record.id = id;
        // Only operation names and an explicitly requested echo are displayed;
        // credentials and arbitrary request JSON never enter the analyzer log.
        for (size_t i = 0; i < op.size() && i + 1 < sizeof(record.operation); ++i) {
            const unsigned char c = op[i];
            record.operation[i] = c >= 32 && c <= 126 ? c : '?';
            record.operation[i + 1] = '\0';
        }
        if (op.starts_with("ble.gatt.")) {
#if !CONFIG_M5_CONNECTIVITY_BLE_ENABLED
            fail("disabled"); return;
#else
            // Attribute values and notifications are private; radio queries
            // require the same authentication as gateway mutations.
            char token[33]{};
            if (origin != Origin::serial && (!copy_string(get("token"), token, sizeof(token)) ||
                !constant_time_equal(token, settings.token))) { fail("unauthorized"); return; }
            gateway::dispatch(root.get(), id, ble,
                [](void* context, const gateway::Request& request, uint32_t request_id, uint32_t* ticket) {
                    ControlRequest control{}; control.action = ControlAction::ble_gatt; control.gatt = request;
                    return static_cast<Impl*>(context)->enqueue(control, request_id, ticket);
                }, this, output, capacity);
            return;
#endif
        }
        Json response(cJSON_CreateObject(), cJSON_Delete);
        if (!response) { fail("no_memory"); return; }
        if (!cJSON_AddNumberToObject(response.get(), "v", 1) ||
            !cJSON_AddNumberToObject(response.get(), "id", id) ||
            !cJSON_AddBoolToObject(response.get(), "ok", true)) {
            fail("no_memory"); return;
        }
        bool built = true;
        auto number = [&](const char* key, double value) {
            built = cJSON_AddNumberToObject(response.get(), key, value) != nullptr && built;
        };
        auto string = [&](const char* key, const char* value) {
            // SSIDs are arbitrary octets. Preserve valid UTF-8; replace invalid
            // bytes for JSON display without changing the stored scan target.
            char safe[129]{};
            copy_display_utf8(value, safe, sizeof(safe));
            built = cJSON_AddStringToObject(response.get(), key, safe) != nullptr && built;
        };
        if (op == "help") {
            char topic[16]{};
            if (get("topic") && !copy_string(get("topic"), topic, sizeof(topic))) { fail("invalid_topic"); return; }
            const char* help = nullptr;
            if (!topic[0]) help = "help [wifi|ble|gatt|protocol]; status; ping; traffic; capabilities; command TICKET. Quote names/passwords with spaces. No device input echo/history.";
            else if (!strcmp(topic, "wifi")) help = "wifi status|scan|results [INDEX]|saved [SLOT]|on|off|disconnect|reconnect|remember; wifi connect SSID PASSWORD [--remember|--temporary]; wifi use SLOT; wifi forget SLOT|all. Empty password: \"\". Default: remember after DHCP; 4 stable slots (0..3).";
            else if (!strcmp(topic, "ble")) help = "ble status|scan|results [INDEX]|peer [INDEX]|saved [SLOT]|bonds [INDEX]|on|off|disconnect|reconnect|remember; ble connect ADDRESS TYPE [--remember|--temporary]; ble use SLOT; ble forget SLOT|all; ble unpair ADDRESS TYPE|all. TYPE: public/random/public-id/random-id (0..3). Forget removes profile; unpair removes security keys.";
            else if (!strcmp(topic, "protocol")) help = "Input: CLI or one API v1 JSON object per LF/CRLF (JSON <=256 bytes). Output: RS (0x1e), JSON, LF; ignore logs outside records. accepted + ticket means queued; command TICKET reports applied/error. Poll wifi/ble status for actual link and memory result. Physical USB is trusted; radio mutations require token. Tickets expire after 16 accepted commands or reboot.";
            else if (!strcmp(topic, "gatt")) help = "gatt status; gatt services|mtu|pair GENERATION; gatt characteristics|descriptors GENERATION HANDLE END; gatt read GENERATION HANDLE; gatt write GENERATION HANDLE HEX; gatt subscribe GENERATION CCCD MODE (0=off,1=notify,2=indicate); gatt result TICKET [INDEX [OFFSET]]; gatt events GENERATION [AFTER]. Poll result until complete/failed; generation comes from status.";
            else { fail("invalid_topic"); return; }
            built = cJSON_AddStringToObject(response.get(), "help", help) != nullptr;
        } else if (op == "capabilities") {
            number("api", 1); number("request_bytes", kMaxRequestBytes);
            number("response_bytes", capacity - 1); number("memory_slots", kMemorySlots);
            number("command_history", CommandHistory::capacity); number("queue_capacity", kCommandCapacity);
#if CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
            number("wifi", 1);
#else
            number("wifi", 0);
#endif
#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED
            number("ble", 1);
            number("ble_gateway", 1);
#else
            number("ble", 0);
            number("ble_gateway", 0);
#endif
            string("serial_framing", "RS-JSON-LF"); string("auth", origin == Origin::serial ? "physical" : "token");
        } else if (op == "command.result") {
            uint32_t ticket;
            if (!unsigned_json(get("ticket"), UINT32_MAX, ticket) || !ticket) { fail("invalid_ticket"); return; }
            const auto result = command_result(ticket);
            if (!result.ticket) { fail("unknown_ticket"); return; }
            number("ticket", ticket); number("request_id", result.id);
            string("state", !result.applied ? "queued" : result.error ? "failed" : "applied");
            number("error_code", result.error); string("error_name", esp_err_to_name(result.error));
        } else if (op == "wifi.saved" || op == "ble.saved") {
            uint32_t index = 0;
            if (get("index") && !unsigned_json(get("index"), kMemorySlots - 1, index)) { fail("invalid_index"); return; }
            const auto saved = memory_snapshot();
            const bool is_wifi = op == "wifi.saved";
            uint32_t mask = 0, count = 0;
            for (size_t i = 0; i < kMemorySlots; ++i) {
                if (is_wifi ? saved.wifi_ssids[i][0] != 0 : saved.ble[i].address[0] != 0) { mask |= 1U << i; ++count; }
            }
            const auto preferred = is_wifi ? saved.preferred_wifi : saved.preferred_ble;
            number("capacity", kMemorySlots); number("count", count); number("used_mask", mask);
            number("preferred", preferred == kNoSlot ? -1 : preferred);
            number("slot", index); number("occupied", (mask >> index) & 1);
            if (is_wifi) string("ssid", saved.wifi_ssids[index]);
            else { string("address", saved.ble[index].address); number("address_type", saved.ble[index].address_type); }
        } else if (op == "wifi.status") {
            const auto data = snapshot(); const auto radio = wifi_diagnostics(); const auto saved = memory_snapshot();
            string("state", wifi_state_name(data.wifi.state)); string("ssid", data.wifi.ssid);
            string("ip", data.wifi.address); number("enabled", radio.enabled); number("scanning", radio.scanning);
            number("reason", data.wifi.disconnect_reason); number("rssi", data.wifi.rssi);
            number("remember_pending", saved.wifi_pending); number("memory_error", saved.wifi_error);
            string("memory_error_name", esp_err_to_name(saved.wifi_error));
        } else if (op == "ble.status") {
            const auto data = ble_diagnostics(); const auto saved = memory_snapshot();
            number("enabled", data.enabled); number("scanning", data.scanning); number("switching", data.switching);
            number("connecting", data.connecting); number("connected", data.peer_connected);
            string("address", data.peer_address); number("address_type", data.peer_address_type);
            string("identity", data.peer_identity); number("identity_type", data.peer_identity_type);
            number("error", data.peer_error); number("encrypted", data.peer_encrypted); number("bonded", data.peer_bonded);
            number("remember_pending", saved.ble_pending); number("memory_error", saved.ble_error);
            string("memory_error_name", esp_err_to_name(saved.ble_error));
        } else if (op == "ble.bonds") {
            uint32_t index = 0;
            if (get("index") && !unsigned_json(get("index"), 255, index)) { fail("invalid_index"); return; }
            const auto data = ble_diagnostics();
            number("count", data.bond_count); number("error", data.bond_error);
            number("generation", data.bond_generation);
            if (index < data.bond_count) {
                number("index", index); string("address", data.bonds[index].address);
                number("address_type", data.bonds[index].address_type);
            } else if (index != 0) { fail("invalid_index"); return; }
        } else if (op == "ping") {
            string("reply", "pong");
        } else if (op == "echo") {
            char echo[129]{};
            if (!copy_string(get("data"), echo, sizeof(echo))) { fail("invalid_data"); return; }
            string("data", echo);
            record.has_echo = true;
            for (size_t i = 0; echo[i] && i + 1 < sizeof(record.echo); ++i) {
                const unsigned char c = echo[i];
                record.echo[i] = c >= 32 && c <= 126 ? c : '.';
            }
        } else if (op == "status") {
            const auto current = connectivity::snapshot();
            string("wifi", wifi_state_name(current.wifi.state));
            string("ip", current.wifi.address);
            string("ap", current.wifi.ap_ssid);
            string("ap_ip", current.wifi.ap_address);
            number("rssi", current.wifi.rssi);
            number("reconnects", current.wifi.reconnects);
            number("reason", current.wifi.disconnect_reason);
            string("ble", current.ble.connected ? "connected" :
                   current.ble.advertising ? "advertising" : current.ble.enabled ? "starting" : "off");
            number("mtu", current.ble.mtu);
            number("ble_secure", current.ble.encrypted ? 1 : 0);
            number("ble_bonded", current.ble.bonded ? 1 : 0);
            number("accepted", current.accepted);
            number("rejected", current.rejected);
            number("completed", current.completed);
            number("last_id", current.last_id);
            number("last_error", current.last_error);
            number("heap", esp_get_free_heap_size());
            number("internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            number("dma_largest", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            number("uptime_s", esp_timer_get_time() / 1000000ULL);
        } else if (op == "telemetry") {
            const auto motion = model::MotionState::instance().snapshot();
            number("valid", motion.valid ? 1 : 0);
            number("samples", motion.sample_count);
            number("roll", motion.roll_deg);
            number("pitch", motion.pitch_deg);
            number("yaw", motion.yaw_deg);
        } else if (op == "wifi.scan.results" || op == "ble.scan.results" || op == "ble.peer") {
            const cJSON* index_json = get("index");
            unsigned index = 0;
            if (index_json) {
                if (!cJSON_IsNumber(index_json) || !std::isfinite(index_json->valuedouble) ||
                    index_json->valuedouble < 0 || index_json->valuedouble > 255 ||
                    floor(index_json->valuedouble) != index_json->valuedouble) {
                    fail("invalid_index"); return;
                }
                index = static_cast<unsigned>(index_json->valuedouble);
            }
            if (op == "wifi.scan.results") {
                const auto data = wifi_diagnostics();
                number("enabled", data.enabled); number("scanning", data.scanning);
                number("generation", data.generation); number("count", data.count);
                number("total", data.total_found); number("error", data.scan_error);
                number("current_channel", data.channel); number("ap_clients", data.ap_clients);
                if (index < data.count) {
                    const auto& row = data.networks[index];
                    number("index", index); string("ssid", row.ssid); string("bssid", row.bssid);
                    number("rssi", row.rssi); number("channel", row.channel); number("auth", row.auth);
                } else if (index != 0) { fail("invalid_index"); return; }
            } else if (op == "ble.scan.results") {
                const auto data = ble_diagnostics();
                number("enabled", data.enabled); number("scanning", data.scanning);
                number("generation", data.generation); number("count", data.count);
                number("total", data.total_found); number("error", data.scan_error);
                if (index < data.count) {
                    const auto& row = data.devices[index];
                    number("index", index); string("name", row.name); string("address", row.address);
                    number("rssi", row.rssi); number("address_type", row.address_type);
                    number("connectable", row.connectable);
                } else if (index != 0) { fail("invalid_index"); return; }
            } else {
                const auto data = ble_diagnostics();
                number("enabled", data.enabled); number("connecting", data.connecting);
                number("connected", data.peer_connected); string("address", data.peer_address);
                string("name", data.peer_name); number("rssi", data.peer_rssi);
                number("mtu", data.peer_mtu); number("error", data.peer_error);
                number("service_count", data.service_count);
                if (index < data.service_count) { number("index", index); string("service", data.services[index]); }
                else if (index != 0) { fail("invalid_index"); return; }
            }
        } else if (op == "traffic") {
            const auto data = traffic_snapshot();
            number("http_requests", data.http_requests); number("ble_requests", data.ble_requests);
            number("serial_requests", data.serial_requests);
            number("rx_bytes", data.rx_bytes); number("tx_bytes", data.tx_bytes);
            string("transport", data.last_transport); string("operation", data.last_operation);
            number("request_id", data.last_request_id); number("success", data.last_ok);
            string("echo", data.echo);
        } else {
            ControlRequest command{};
            if (op == "wifi.configure") command.action = ControlAction::wifi_configure;
            else if (op == "wifi.connect") command.action = ControlAction::wifi_connect;
            else if (op == "wifi.remember") command.action = ControlAction::wifi_remember;
            else if (op == "wifi.forget") command.action = ControlAction::wifi_forget;
            else if (op == "wifi.use") command.action = ControlAction::wifi_use;
            else if (op == "wifi.clear") command.action = ControlAction::wifi_clear;
            else if (op == "wifi.scan") command.action = ControlAction::wifi_scan;
            else if (op == "wifi.reconnect") command.action = ControlAction::wifi_reconnect;
            else if (op == "wifi.disconnect") command.action = ControlAction::wifi_disconnect;
            else if (op == "wifi.enable") command.action = ControlAction::wifi_enable;
            else if (op == "wifi.disable") command.action = ControlAction::wifi_disable;
            else if (op == "ble.scan") command.action = ControlAction::ble_scan;
            else if (op == "ble.connect") command.action = ControlAction::ble_connect;
            else if (op == "ble.disconnect") command.action = ControlAction::ble_disconnect;
            else if (op == "ble.enable") command.action = ControlAction::ble_enable;
            else if (op == "ble.disable") command.action = ControlAction::ble_disable;
            else if (op == "ble.remember") command.action = ControlAction::ble_remember;
            else if (op == "ble.forget") command.action = ControlAction::ble_forget;
            else if (op == "ble.use") command.action = ControlAction::ble_use;
            else if (op == "ble.reconnect") command.action = ControlAction::ble_reconnect;
            else if (op == "ble.unpair") command.action = ControlAction::ble_unpair;
            else { fail("unknown_operation"); return; }
            char token[33]{};
            if (origin != Origin::serial && (!copy_string(get("token"), token, sizeof(token)) ||
                !constant_time_equal(token, settings.token))) { fail("unauthorized"); return; }
            const bool wifi_connect = command.action == ControlAction::wifi_connect || command.action == ControlAction::wifi_configure;
            const bool ble_connect = command.action == ControlAction::ble_connect;
            const bool forget = command.action == ControlAction::wifi_forget || command.action == ControlAction::ble_forget;
            const bool use = command.action == ControlAction::wifi_use || command.action == ControlAction::ble_use;
            const bool unpair = command.action == ControlAction::ble_unpair;
            // Strict mutation schemas catch typos before changing radio state.
            for (auto* field = root->child; field; field = field->next) {
                const std::string_view key(field->string);
                if (key == "v" || key == "id" || key == "op" || key == "token") continue;
                if (wifi_connect && (key == "ssid" || key == "password")) continue;
                if ((wifi_connect || ble_connect) && key == "remember" && op != "wifi.configure") continue;
                if ((ble_connect || unpair) && (key == "address" || key == "address_type")) continue;
                if ((forget || use) && key == "slot") continue;
                if ((forget || unpair) && key == "all") continue;
                fail("unknown_field"); return;
            }
            // Keep legacy radio BLE requests temporary unless they opt in.
            command.remember = op == "wifi.connect" || origin == Origin::serial;
            if (get("remember")) {
                if (!cJSON_IsBool(get("remember"))) { fail("invalid_remember"); return; }
                command.remember = cJSON_IsTrue(get("remember"));
            }
            if (get("all")) {
                if (!cJSON_IsBool(get("all"))) { fail("invalid_all"); return; }
                command.all = cJSON_IsTrue(get("all"));
            }
            if (use || forget) {
                uint32_t slot;
                if (command.all && get("slot")) { fail("invalid_slot"); return; }
                if (!command.all) {
                    if (!unsigned_json(get("slot"), kMemorySlots - 1, slot)) { fail("invalid_slot"); return; }
                    command.slot = static_cast<uint8_t>(slot);
                }
            }
            if (unpair && command.all && (get("address") || get("address_type"))) { fail("invalid_peer"); return; }
            if (wifi_connect &&
                (!copy_string(get("ssid"), command.wifi.ssid, sizeof(command.wifi.ssid)) ||
                 !copy_string(get("password"), command.wifi.password, sizeof(command.wifi.password)) ||
                 !valid_credentials(command.wifi))) { fail("invalid_credentials"); return; }
            if (ble_connect || (unpair && !command.all)) {
                uint32_t type;
                if (!copy_string(get("address"), command.address, sizeof(command.address)) ||
                    !valid_ble_address(command.address) || !unsigned_json(get("address_type"), 3, type)) { fail("invalid_peer"); return; }
                command.address_type = static_cast<uint8_t>(type);
            }
            uint32_t ticket = 0;
            const esp_err_t result = enqueue(command, id, &ticket);
            snprintf(output, capacity,
                     "{\"v\":1,\"id\":%lu,\"ok\":true,\"result\":\"accepted\",\"ticket\":%lu}",
                     static_cast<unsigned long>(id), static_cast<unsigned long>(ticket));
            if (result != ESP_OK) fail(result == ESP_ERR_NOT_SUPPORTED ? "disabled" :
                                      result == ESP_ERR_INVALID_ARG ? "invalid_command" : "busy");
            return;
        }
        if (!built) { fail("no_memory"); return; }
        if (!cJSON_PrintPreallocated(response.get(), output, capacity, false)) {
            fail("response_too_large");
        }
    }
};

Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() = default;

esp_err_t Service::initialize()
{
    auto& self = *impl_;
    if (self.initialized) return ESP_OK;
    if (!self.queue) return ESP_ERR_NO_MEM;
    // Do not erase NVS automatically: it may contain other modules' data.
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result;
    self.nvs_ready = true;
    result = self.load();
    if (result != ESP_OK) return result;
    result = esp_netif_init();
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result == ESP_OK) self.event_loop_owned = true;
    else if (result != ESP_ERR_INVALID_STATE) return result;
    uint8_t mac[6]{};
    result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) return result;
    char name[25]{};
    snprintf(name, sizeof(name), "M5StickS3-%02X%02X%02X", mac[3], mac[4], mac[5]);
#if CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
    self.wifi_attempted = true;
    result = self.wifi.start(Impl::handle_wifi, &self, name, self.settings.ap_password, self.current_wifi);
    if (result != ESP_OK) return result;
    self.wifi_started = true;
#endif
#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED
    self.ble_attempted = true;
    result = self.ble.start(name, Impl::handle_ble, &self);
    if (result != ESP_OK) return result;
    self.ble_started = true;
#endif
    result = self.console.start_console(Impl::handle_serial, &self);
    if (result != ESP_OK) return result;
    self.initialized = true;
    portENTER_CRITICAL(&state_lock);
    memcpy(local_ap_password, self.settings.ap_password, sizeof(local_ap_password));
    portEXIT_CRITICAL(&state_lock);
    self.update();
    ESP_LOGI(kTag, "ready: %s (API v1)", name);
    // Physical USB console is the bootstrap channel; never return these over radio.
    ESP_LOGI(kTag, "local setup: AP password=%s", self.settings.ap_password);
    ESP_LOGI(kTag, "local setup: API token=%s", self.settings.token);
    return ESP_OK;
}

void Service::process()
{
    auto& self = *impl_;
    if (!self.initialized) return;
    // AppManager dispatches process only after every module initialized.
    // Do not acknowledge commands while a later module can still roll back.
    self.accepting.store(true, std::memory_order_release);
    portENTER_CRITICAL(&state_lock);
    local_context = &self;
    local_submit = Impl::submit_local;
    portEXIT_CRITICAL(&state_lock);
    self.wifi.process();
    self.ble.process();
    Command command{};
    // One command per tick bounds application-loop latency and flash activity.
    if (xQueueReceive(self.queue, &command, 0) == pdPASS) {
        const esp_err_t result = self.execute(command.control, command.ticket);
        self.state.last_id = command.id;
        self.state.last_error = result;
        ++self.state.completed;
        portENTER_CRITICAL(&history_lock);
        history.finish(command.ticket, result);
        portEXIT_CRITICAL(&history_lock);
        ESP_LOGI(kTag, "command %lu completed: %s",
                 static_cast<unsigned long>(command.id), esp_err_to_name(result));
    }
    self.process_memory();
    self.update();
}

esp_err_t Service::deinitialize()
{
    auto& self = *impl_;
    self.accepting.store(false, std::memory_order_release);
    portENTER_CRITICAL(&state_lock);
    local_submit = nullptr;
    local_context = nullptr;
    memset(local_ap_password, 0, sizeof(local_ap_password));
    portEXIT_CRITICAL(&state_lock);
    // Keep the facade and its queue alive until every transport callback exits.
    const auto console_result = self.console.stop_console();
    if (console_result != ESP_OK) return console_result;
    if (self.ble_attempted) {
        const esp_err_t result = self.ble.stop();
        if (result != ESP_OK) return result;
        self.ble_attempted = self.ble_started = false;
    }
    if (self.wifi_attempted) {
        const esp_err_t result = self.wifi.stop();
        if (result != ESP_OK) return result;
        self.wifi_attempted = self.wifi_started = false;
    }
    if (self.event_loop_owned) {
        const esp_err_t result = esp_event_loop_delete_default();
        if (result != ESP_OK) return result;
        self.event_loop_owned = false;
    }
    if (self.storage) { nvs_close(self.storage); self.storage = 0; }
    // NVS and esp_netif are process-wide IDF facilities. Leave initialized to
    // avoid invalidating other modules; their init calls support subsequent starts.
    self.nvs_ready = false;
    self.initialized = false;
    self.wifi_remember_deadline = self.ble_remember_deadline = 0;
    self.wifi_memory_error = self.ble_memory_error = 0;
    self.current_wifi = {};
    self.pending_ble = {};
    if (self.queue) xQueueReset(self.queue);
    self.update();
    return ESP_OK;
}
} // namespace connectivity
