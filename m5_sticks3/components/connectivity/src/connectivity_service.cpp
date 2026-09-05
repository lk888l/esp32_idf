#include "connectivity_service.hpp"
#include "connectivity_runtime.hpp"
#include "connectivity_policy.hpp"
#include "wifi_transport.hpp"
#include "ble_transport.hpp"

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
    WifiCredentials wifi{};
    uint32_t id = 0;
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
// Reject deep nesting before cJSON recursion, embedded NULs, and escaped NULs
// (cJSON strings cannot represent them without truncation).
bool bounded_json(const char* text, size_t length)
{
    if (!text || !length || length > kMaxRequestBytes || memchr(text, 0, length)) return false;
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
    Settings settings{};
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

    Impl()
    {
        queue = xQueueCreateStatic(kCommandCapacity, sizeof(Command),
                                   queue_storage.data(), &queue_state);
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
            return persist(settings);
        }
        if (result != ESP_OK) return result;
        if (size != sizeof(settings) || settings.version != 1 ||
            !hex_secret(settings.token, 32) || !hex_secret(settings.ap_password, 16) ||
            (settings.wifi.ssid[0] && !valid_credentials(settings.wifi))) {
            return ESP_ERR_INVALID_VERSION;
        }
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
    }

    static void handle(void* context, const char* input, size_t length,
                       char* output, size_t capacity)
    {
        auto& self = *static_cast<Impl*>(context);
        self.request(input, length, output, capacity);
    }

    void request(const char* input, size_t length, char* output, size_t capacity)
    {
        uint32_t id = 0;
        const auto fail = [&](const char* why) {
            rejected.fetch_add(1, std::memory_order_relaxed);
            error_response(output, capacity, id, why);
        };
        if (!output || capacity < 96) return;
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
            built = cJSON_AddStringToObject(response.get(), key, value) != nullptr && built;
        };
        if (op == "ping") {
            string("reply", "pong");
        } else if (op == "echo") {
            char echo[129]{};
            if (!copy_string(get("data"), echo, sizeof(echo))) { fail("invalid_data"); return; }
            string("data", echo);
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
        } else if (op == "wifi.configure" || op == "wifi.clear") {
            char token[33]{};
            if (!copy_string(get("token"), token, sizeof(token)) ||
                !constant_time_equal(token, settings.token)) { fail("unauthorized"); return; }
#if CONFIG_M5_CONNECTIVITY_WIFI_ENABLED
            Command command{};
            command.id = id;
            if (op == "wifi.configure" &&
                (!copy_string(get("ssid"), command.wifi.ssid, sizeof(command.wifi.ssid)) ||
                 !copy_string(get("password"), command.wifi.password, sizeof(command.wifi.password)) ||
                 !valid_credentials(command.wifi))) {
                fail("invalid_credentials"); return;
            }
            // Serialize before enqueue: an error reply must never conceal a side effect.
            snprintf(output, capacity,
                     "{\"v\":1,\"id\":%lu,\"ok\":true,\"result\":\"accepted\"}",
                     static_cast<unsigned long>(id));
            if (!queue || xQueueSend(queue, &command, 0) != pdPASS) { fail("busy"); return; }
            accepted.fetch_add(1, std::memory_order_relaxed);
            return;
#else
            fail("wifi_disabled"); return;
#endif
        } else {
            fail("unknown_operation"); return;
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
#if !CONFIG_M5_CONNECTIVITY_WIFI_ENABLED && !CONFIG_M5_CONNECTIVITY_BLE_ENABLED
    return ESP_OK;
#else
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
    result = self.wifi.start(Impl::handle, &self, name, self.settings.ap_password, self.settings.wifi);
    if (result != ESP_OK) return result;
    self.wifi_started = true;
#endif
#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED
    self.ble_attempted = true;
    result = self.ble.start(name, Impl::handle, &self);
    if (result != ESP_OK) return result;
    self.ble_started = true;
#endif
    self.initialized = true;
    self.update();
    ESP_LOGI(kTag, "ready: %s (API v1)", name);
    // Physical USB console is the bootstrap channel; never return these over radio.
    ESP_LOGI(kTag, "local setup: AP password=%s", self.settings.ap_password);
    ESP_LOGI(kTag, "local setup: API token=%s", self.settings.token);
    return ESP_OK;
#endif
}

void Service::process()
{
    auto& self = *impl_;
    if (!self.initialized) return;
    // AppManager dispatches process only after every module initialized.
    // Do not acknowledge commands while a later module can still roll back.
    self.accepting.store(true, std::memory_order_release);
    self.wifi.process();
    Command command{};
    // One command per tick bounds application-loop latency and flash activity.
    if (xQueueReceive(self.queue, &command, 0) == pdPASS) {
        esp_err_t result = ESP_OK;
        if (memcmp(&command.wifi, &self.settings.wifi, sizeof(command.wifi)) != 0) {
            Settings updated = self.settings;
            updated.wifi = command.wifi;
            // Commit first: accepted settings survive power loss during reconnect.
            result = self.persist(updated);
            if (result == ESP_OK) {
                self.settings.wifi = updated.wifi;
            }
        }
        // Reapply even identical persisted credentials, allowing recovery after a
        // previous driver failure without another flash write.
        if (result == ESP_OK) result = self.wifi.configure(command.wifi);
        self.state.last_id = command.id;
        self.state.last_error = result;
        ++self.state.completed;
        ESP_LOGI(kTag, "command %lu completed: %s",
                 static_cast<unsigned long>(command.id), esp_err_to_name(result));
    }
    self.update();
}

esp_err_t Service::deinitialize()
{
    auto& self = *impl_;
    self.accepting.store(false, std::memory_order_release);
    // Keep the facade and its queue alive until every transport callback exits.
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
    if (self.queue) xQueueReset(self.queue);
    self.update();
    return ESP_OK;
}
} // namespace connectivity
