#include "wireless_esp32/settings_store.hpp"

#include "esp_mac.h"
#include "nvs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wireless_esp32 {
namespace {
constexpr char kNamespace[] = "wireless";

bool valid_text_length(const char* value, std::size_t maximum, bool allow_empty)
{
    if (value == nullptr) {
        return false;
    }
    const std::size_t length = strnlen(value, maximum + 1);
    return length <= maximum && (allow_empty || length != 0);
}

template <std::size_t N>
bool load_string(nvs_handle_t handle, const char* key, std::array<char, N>& output)
{
    std::size_t length = output.size();
    output.fill(0);
    return nvs_get_str(handle, key, output.data(), &length) == ESP_OK &&
           length > 0 && length <= output.size();
}

template <std::size_t N>
void copy_string(std::array<char, N>& output, const char* input)
{
    output.fill(0);
    const std::size_t length = std::min<std::size_t>(std::strlen(input), N - 1);
    std::copy_n(input, length, output.begin());
}

} // namespace

SettingsStore::~SettingsStore()
{
    if (handle_ != 0) {
        nvs_close(static_cast<nvs_handle_t>(handle_));
    }
}

bool SettingsStore::initialize(
    uint32_t ble_passkey,
    const char* ap_password,
    std::span<const uint8_t, WirelessSettings::kControlKeySize> control_key)
{
    if (handle_ != 0 || ble_passkey < 100000 || ble_passkey > 999999 ||
        !valid_text_length(ap_password, 63, false) || std::strlen(ap_password) < 8) {
        return handle_ != 0;
    }
    nvs_handle_t opened = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &opened) != ESP_OK) {
        return false;
    }
    handle_ = opened;

    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        nvs_close(opened);
        handle_ = 0;
        return false;
    }
    std::snprintf(settings_.ap_ssid.data(),
                  settings_.ap_ssid.size(),
                  "HexMotor-C5-%02X%02X%02X",
                  mac[3],
                  mac[4],
                  mac[5]);
    copy_string(settings_.ap_password, ap_password);
    std::copy(control_key.begin(), control_key.end(), settings_.control_key.begin());
    settings_.ble_passkey = ble_passkey;
    load_station();
    return true;
}

void SettingsStore::load_station()
{
    const nvs_handle_t handle = static_cast<nvs_handle_t>(handle_);
    const bool have_ssid = load_string(handle, "sta_ssid", settings_.sta_ssid);
    const bool have_password = load_string(handle, "sta_pass", settings_.sta_password);
    settings_.station_configured = have_ssid && have_password;
    if (!settings_.station_configured) {
        settings_.sta_ssid.fill(0);
        settings_.sta_password.fill(0);
    }
}

bool SettingsStore::update_station(const char* ssid, const char* password)
{
    if (handle_ == 0 || !valid_text_length(ssid, 32, false) ||
        !valid_text_length(password, 63, true)) {
        return false;
    }
    const nvs_handle_t handle = static_cast<nvs_handle_t>(handle_);
    if (nvs_set_str(handle, "sta_ssid", ssid) != ESP_OK ||
        nvs_set_str(handle, "sta_pass", password) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) {
        return false;
    }
    copy_string(settings_.sta_ssid, ssid);
    copy_string(settings_.sta_password, password);
    settings_.station_configured = true;
    return true;
}

} // namespace wireless_esp32
