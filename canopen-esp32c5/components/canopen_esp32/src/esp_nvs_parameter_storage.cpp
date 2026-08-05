#include "canopen_esp32/esp_nvs_parameter_storage.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace canopen_esp32 {
namespace {
constexpr char kTag[] = "canopen_nvs";

bool valid_node_id(uint8_t node_id)
{
    return node_id >= 1 && node_id <= 127;
}
} // namespace

EspNvsParameterStorage::EspNvsParameterStorage(uint8_t default_node_id)
    : startup_node_id_(default_node_id)
{
    if (!valid_node_id(default_node_id)) {
        ESP_LOGE(kTag, "invalid configured default node ID %u", default_node_id);
        return;
    }

    const esp_err_t init_result = nvs_flash_init();
    if (init_result != ESP_OK) {
        ESP_LOGE(kTag,
                 "NVS initialization failed: %s; refusing to start without persistence",
                 esp_err_to_name(init_result));
        return;
    }

    const esp_err_t open_result = nvs_open(kNamespace, NVS_READWRITE, &handle_);
    if (open_result != ESP_OK) {
        ESP_LOGE(kTag, "opening NVS namespace failed: %s", esp_err_to_name(open_result));
        return;
    }

    uint8_t persisted_node_id = 0;
    const esp_err_t read_result = nvs_get_u8(handle_, kNodeIdKey, &persisted_node_id);
    if (read_result == ESP_OK) {
        if (valid_node_id(persisted_node_id)) {
            startup_node_id_ = persisted_node_id;
            stored_node_id_ = persisted_node_id;
            restored_ = true;
        } else {
            ESP_LOGW(kTag,
                     "ignoring invalid persisted node ID %u; using configured default 0x%02X",
                     persisted_node_id,
                     default_node_id);
        }
    } else if (read_result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(kTag, "reading persisted node ID failed: %s", esp_err_to_name(read_result));
        nvs_close(handle_);
        handle_ = 0;
        return;
    }

    ready_ = true;
}

EspNvsParameterStorage::~EspNvsParameterStorage()
{
    if (handle_ != 0) {
        nvs_close(handle_);
    }
}

bool EspNvsParameterStorage::store_node_id(uint8_t node_id)
{
    if (!ready_ || !valid_node_id(node_id)) {
        return false;
    }
    if (node_id == stored_node_id_) {
        return true;
    }

    const esp_err_t set_result = nvs_set_u8(handle_, kNodeIdKey, node_id);
    if (set_result != ESP_OK) {
        ESP_LOGE(kTag, "staging node ID failed: %s", esp_err_to_name(set_result));
        return false;
    }
    const esp_err_t commit_result = nvs_commit(handle_);
    if (commit_result != ESP_OK) {
        ESP_LOGE(kTag, "committing node ID failed: %s", esp_err_to_name(commit_result));
        return false;
    }

    stored_node_id_ = node_id;
    ESP_LOGI(kTag,
             "saved node ID 0x%02X; it will become active after power-cycle or CPU restart",
             node_id);
    return true;
}

} // namespace canopen_esp32
