idf_component_register(
    SRCS "src/connectivity_service.cpp" "src/wifi_transport.cpp" "src/ble_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app model esp_event esp_netif esp_wifi esp_http_server bt nvs_flash
             esp_timer esp_system esp_hw_support bootloader_support json
)
