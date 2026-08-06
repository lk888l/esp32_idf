#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wireless_esp32 {

struct WirelessSettings {
    static constexpr std::size_t kSsidCapacity = 33;
    static constexpr std::size_t kPasswordCapacity = 64;
    static constexpr std::size_t kControlKeySize = 32;

    std::array<char, kSsidCapacity> ap_ssid{};
    std::array<char, kPasswordCapacity> ap_password{};
    std::array<char, kSsidCapacity> sta_ssid{};
    std::array<char, kPasswordCapacity> sta_password{};
    std::array<uint8_t, kControlKeySize> control_key{};
    uint32_t ble_passkey = 0;
    bool station_configured = false;
};

class SettingsStore {
public:
    SettingsStore() = default;
    ~SettingsStore();

    bool initialize(uint32_t ble_passkey,
                    const char* ap_password,
                    std::span<const uint8_t, WirelessSettings::kControlKeySize> control_key);
    bool update_station(const char* ssid, const char* password);
    [[nodiscard]] const WirelessSettings& settings() const { return settings_; }
    [[nodiscard]] bool ready() const { return handle_ != 0; }

private:
    void load_station();

    WirelessSettings settings_{};
    uint32_t handle_ = 0;
};

} // namespace wireless_esp32
