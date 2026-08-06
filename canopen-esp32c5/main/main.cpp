#include "app/app_manager.hpp"
#include "canopen_esp32/canopen_module.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_WIRELESS_GATEWAY_ENABLED
#include "wireless_esp32/wireless_module.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
constexpr char kTag[] = "main";

canopen_esp32::ModuleConfig make_canopen_config()
{
    canopen_esp32::ModuleConfig config{};
    config.twai.tx_pin = static_cast<gpio_num_t>(CONFIG_CANOPEN_TWAI_TX_GPIO);
    config.twai.rx_pin = static_cast<gpio_num_t>(CONFIG_CANOPEN_TWAI_RX_GPIO);
    config.twai.tx_queue_depth = CONFIG_CANOPEN_TWAI_TX_QUEUE_DEPTH;
    config.twai.rx_queue_depth = CONFIG_CANOPEN_TWAI_RX_QUEUE_DEPTH;
    config.gateway.ingress_queue_depth = CONFIG_CANOPEN_GATEWAY_INGRESS_QUEUE_DEPTH;
    config.gateway.monitor_queue_depth = CONFIG_CANOPEN_GATEWAY_MONITOR_QUEUE_DEPTH;
    config.profile.node_id = CONFIG_CANOPEN_NODE_ID;
    config.profile.producer_heartbeat_ms = CONFIG_CANOPEN_HEARTBEAT_PERIOD_MS;
    config.profile.device_type = 0;
    config.profile.identity = {
        .vendor_id = CONFIG_CANOPEN_VENDOR_ID,
        .product_code = CONFIG_CANOPEN_PRODUCT_CODE,
        .revision_number = CONFIG_CANOPEN_REVISION_NUMBER,
        .serial_number = CONFIG_CANOPEN_SERIAL_NUMBER,
    };
    config.profile.device_name = "ESP32-C5 CANopen-FD Wireless Node";
    config.profile.hardware_version = "ESP32-C5";
    config.profile.software_version = "0.3.0";
    config.profile.can_fd_pdo = true;
    config.task_stack_size = CONFIG_CANOPEN_TASK_STACK_SIZE;
    config.task_priority = CONFIG_CANOPEN_TASK_PRIORITY;
    return config;
}

#if CONFIG_WIRELESS_GATEWAY_ENABLED
int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool decode_control_key(std::array<uint8_t, wireless_esp32::WirelessSettings::kControlKeySize>& key)
{
    constexpr const char* encoded = CONFIG_WIRELESS_TCP_CONTROL_KEY_HEX;
    if (std::strlen(encoded) != key.size() * 2) {
        return false;
    }
    bool any_nonzero = false;
    for (std::size_t index = 0; index < key.size(); ++index) {
        const int high = hex_digit(encoded[index * 2]);
        const int low = hex_digit(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        key[index] = static_cast<uint8_t>((high << 4U) | low);
        any_nonzero = any_nonzero || key[index] != 0;
    }
    return any_nonzero;
}

wireless_esp32::WirelessModuleConfig make_wireless_config()
{
    wireless_esp32::WirelessModuleConfig config{};
    config.wifi.ap_channel = CONFIG_WIRELESS_AP_CHANNEL;
    config.wifi.max_ap_clients = CONFIG_WIRELESS_AP_MAX_CLIENTS;
    config.tcp.port = CONFIG_WIRELESS_TCP_PORT;
    config.tcp.discovery_port = CONFIG_WIRELESS_DISCOVERY_PORT;
    config.tcp.max_clients = CONFIG_WIRELESS_TCP_MAX_CLIENTS;
    config.tcp.task_stack_size = CONFIG_WIRELESS_TCP_TASK_STACK_SIZE;
    config.tcp.task_priority = CONFIG_WIRELESS_TCP_TASK_PRIORITY;
    config.canopen_node_id = CONFIG_CANOPEN_NODE_ID;
    config.ble_passkey = CONFIG_WIRELESS_BLE_PASSKEY;
    config.service_task_stack_size = CONFIG_WIRELESS_SERVICE_TASK_STACK_SIZE;
    config.service_task_priority = CONFIG_WIRELESS_SERVICE_TASK_PRIORITY;
    std::snprintf(config.ble.device_name.data(),
                  config.ble.device_name.size(),
                  "HexMotor-C5-%02X",
                  CONFIG_CANOPEN_NODE_ID);
    std::snprintf(config.ap_password.data(),
                  config.ap_password.size(),
                  "%s",
                  CONFIG_WIRELESS_AP_PASSWORD);
    std::snprintf(config.firmware_version.data(),
                  config.firmware_version.size(),
                  "0.3.0");
    if (!decode_control_key(config.control_key)) {
        config.control_key.fill(0);
    }
    return config;
}
#endif
} // namespace

extern "C" void app_main()
{
    app::Manager manager;
    auto canopen =
        std::make_unique<canopen_esp32::CanopenModule>(make_canopen_config());
#if CONFIG_WIRELESS_GATEWAY_ENABLED
    canopen_esp32::EspCanGateway* can_gateway = &canopen->gateway();
#endif
    const auto canopen_registration = manager.register_module(std::move(canopen));
    if (!canopen_registration) {
        ESP_LOGE(kTag, "CANopen module registration failed");
        return;
    }

#if CONFIG_WIRELESS_GATEWAY_ENABLED
    auto wireless_config = make_wireless_config();
    const std::size_t ap_password_length = std::strlen(wireless_config.ap_password.data());
    const bool valid_control_key = std::any_of(
        wireless_config.control_key.begin(),
        wireless_config.control_key.end(),
        [](uint8_t value) { return value != 0; });
    if (ap_password_length < 8 || ap_password_length > 63 ||
        wireless_config.ble_passkey < 100000 ||
        wireless_config.ble_passkey > 999999 || !valid_control_key) {
        ESP_LOGE(kTag,
                 "wireless credentials are invalid; configure AP password, BLE passkey, and 64-digit TCP key");
        return;
    }
    const auto wireless_registration = manager.register_module(
        std::make_unique<wireless_esp32::WirelessModule>(
            wireless_config, *can_gateway));
    if (!wireless_registration) {
        ESP_LOGE(kTag, "wireless module registration failed");
        return;
    }
#endif

    const auto started = manager.initialize_all();
    if (!started) {
        ESP_LOGE(kTag,
                 "module initialization failed: %.*s",
                 static_cast<int>(started.module_name.size()),
                 started.module_name.data());
        return;
    }
    ESP_LOGI(kTag, "CANopen ESP32-C5 framework started");
    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
