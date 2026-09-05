idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS
        "src/bsp_board.cpp"
        "src/bsp_display.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_lcd
        esp_lvgl_port
        esp_timer
        lvgl
)
