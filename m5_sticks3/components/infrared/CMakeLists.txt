idf_component_register(
    SRCS "src/infrared_protocol.cpp" "src/infrared_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_driver_rmt nvs_flash esp_timer
)
