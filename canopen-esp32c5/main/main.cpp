#include "app/app_manager.hpp"
#include "canopen_esp32/canopen_module.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <memory>

namespace {
constexpr char kTag[] = "main";

canopen_esp32::ModuleConfig make_config()
{
    canopen_esp32::ModuleConfig config{};
    config.twai.tx_pin = static_cast<gpio_num_t>(CONFIG_CANOPEN_TWAI_TX_GPIO);
    config.twai.rx_pin = static_cast<gpio_num_t>(CONFIG_CANOPEN_TWAI_RX_GPIO);
    config.twai.tx_queue_depth = CONFIG_CANOPEN_TWAI_TX_QUEUE_DEPTH;
    config.twai.rx_queue_depth = CONFIG_CANOPEN_TWAI_RX_QUEUE_DEPTH;
    config.profile.node_id = CONFIG_CANOPEN_NODE_ID;
    config.profile.producer_heartbeat_ms = CONFIG_CANOPEN_HEARTBEAT_PERIOD_MS;
    config.profile.device_type = 0;
    config.profile.identity = {
        .vendor_id = CONFIG_CANOPEN_VENDOR_ID,
        .product_code = CONFIG_CANOPEN_PRODUCT_CODE,
        .revision_number = CONFIG_CANOPEN_REVISION_NUMBER,
        .serial_number = CONFIG_CANOPEN_SERIAL_NUMBER,
    };
    config.profile.device_name = "ESP32-C5 CANopen-FD Node";
    config.profile.hardware_version = "ESP32-C5";
    config.profile.software_version = "0.2.0";
    config.profile.can_fd_pdo = true;
    config.task_stack_size = CONFIG_CANOPEN_TASK_STACK_SIZE;
    config.task_priority = CONFIG_CANOPEN_TASK_PRIORITY;
    return config;
}
} // namespace

extern "C" void app_main()
{
    app::Manager manager;
    const auto registration =
        manager.register_module(std::make_unique<canopen_esp32::CanopenModule>(make_config()));
    if (!registration) {
        ESP_LOGE(kTag, "module registration failed");
        return;
    }
    const auto started = manager.initialize_all();
    if (!started) {
        ESP_LOGE(kTag, "module initialization failed: %.*s", static_cast<int>(started.module_name.size()), started.module_name.data());
        return;
    }
    ESP_LOGI(kTag, "CANopen ESP32-C5 framework started");
    while (true) {
        manager.process_all();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
