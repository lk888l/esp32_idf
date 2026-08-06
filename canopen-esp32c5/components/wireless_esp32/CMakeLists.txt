set(wireless_sources)
if(CONFIG_WIRELESS_GATEWAY_ENABLED)
    list(APPEND wireless_sources
        "src/settings_store.cpp"
        "src/wifi_manager.cpp"
        "src/tcp_server.cpp"
        "src/ble_server.cpp"
        "src/wireless_module.cpp"
    )
endif()

idf_component_register(
    SRCS ${wireless_sources}
    INCLUDE_DIRS "include"
    REQUIRES
        app
        canopen_esp32
        wireless_protocol
        bt
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        lwip
        mbedtls
        nvs_flash
)
