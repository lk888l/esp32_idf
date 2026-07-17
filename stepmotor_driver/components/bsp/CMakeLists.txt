idf_component_register(
    SRCS
        "src/esp_tb6612_bridge.cpp"
        "src/esp_spi_register_device.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi motor_control
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
