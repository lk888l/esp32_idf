idf_component_register(
    SRCS "src/sleep_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app bsp clock esp_hw_support esp_driver_gpio esp_system
)
