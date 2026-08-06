idf_component_register(
    SRCS
        "src/esp_twai_transport.cpp"
        "src/esp_can_gateway.cpp"
        "src/esp_nvs_parameter_storage.cpp"
        "src/canopen_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app can_transport canopen esp_driver_gpio esp_driver_twai esp_timer nvs_flash
)
