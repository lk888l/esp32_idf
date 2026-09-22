#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "connectivity_types.hpp"

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_FOUND = 0x105, ESP_ERR_NOT_SUPPORTED = 0x106,
    ESP_ERR_TIMEOUT = 0x107, ESP_ERR_INVALID_VERSION = 0x10a,
    ESP_ERR_NVS_NOT_FOUND = 0x1102, ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
inline const char* esp_err_to_name(int error) { return error == 0 ? "ESP_OK" : "MOCK_ERROR"; }
namespace fake {
inline std::map<std::string, std::vector<uint8_t>> nvs, staged;
inline int write_error = 0, commit_error = 0, commits = 0;
inline int64_t now = 1'000'000;
inline connectivity::RequestHandler http = nullptr, ble = nullptr, serial = nullptr;
inline void* context = nullptr;
}
using nvs_handle_t = unsigned;
constexpr int NVS_READWRITE = 1;
inline int nvs_flash_init() { return ESP_OK; }
inline int nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; return ESP_OK; }
inline int nvs_get_blob(nvs_handle_t, const char* key, void* output, size_t* size)
{
    auto it = fake::nvs.find(key);
    if (it == fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < it->second.size()) { *size = it->second.size(); return ESP_ERR_NVS_INVALID_LENGTH; }
    *size = it->second.size(); std::memcpy(output, it->second.data(), *size); return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t, const char* key, const void* input, size_t size)
{
    if (fake::write_error) return fake::write_error;
    const auto* bytes = static_cast<const uint8_t*>(input);
    fake::staged[key] = {bytes, bytes + size}; return ESP_OK;
}
inline int nvs_commit(nvs_handle_t)
{
    if (fake::commit_error) return fake::commit_error;
    for (const auto& [key, bytes] : fake::staged) fake::nvs[key] = bytes;
    fake::staged.clear(); ++fake::commits; return ESP_OK;
}
inline void nvs_close(nvs_handle_t) { fake::staged.clear(); }
inline int esp_netif_init() { return ESP_OK; }
inline int esp_event_loop_create_default() { return ESP_OK; }
inline int esp_event_loop_delete_default() { return ESP_OK; }
constexpr int ESP_MAC_WIFI_STA = 0, MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_DMA = 2;
inline int esp_read_mac(uint8_t* mac, int) { std::memset(mac, 1, 6); return ESP_OK; }
inline void esp_fill_random(void* bytes, size_t size) { std::memset(bytes, 0x21, size); }
inline void bootloader_random_enable() {}
inline void bootloader_random_disable() {}
inline int64_t esp_timer_get_time() { return fake::now; }
inline unsigned esp_get_free_heap_size() { return 200000; }
inline unsigned heap_caps_get_free_size(int) { return 100000; }
inline unsigned heap_caps_get_largest_free_block(int) { return 100000; }
inline void fake_log(const char*, const char*, ...) {}
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
