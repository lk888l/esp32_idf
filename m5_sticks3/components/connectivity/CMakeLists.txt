idf_component_register(
    SRCS "src/connectivity_service.cpp" "src/wifi_transport.cpp" "src/ble_transport.cpp"
         "src/local_console_protocol.cpp" "src/serial_console.cpp"
         "src/web_pairing.cpp"
         "src/ble_gateway.cpp" "src/ble_gatt_client.cpp" "src/ble_gateway_protocol.cpp"
    EMBED_TXTFILES "web/index.html" "web/app.js"
    INCLUDE_DIRS "include"
    REQUIRES app model esp_event esp_netif esp_wifi esp_http_server bt nvs_flash
             esp_timer esp_system esp_hw_support bootloader_support json debug_probe clock pc_monitor
)

# The opt-in UI harness owns stdin in this build; never create two readers.
if(M5_STICKS3_DAP_SMOKE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_STICKS3_DAP_SMOKE_TEST=1)
endif()

if(CONFIG_M5_CONNECTIVITY_WIFI_ENABLED)
    target_sources(${COMPONENT_LIB} PRIVATE "src/web_service.cpp")
endif()
