idf_component_register(
    SRCS
        "src/board_module.cpp"
        "src/connectivity_module.cpp"
        "src/button_module.cpp"
        "src/motion_module.cpp"
        "src/ui_module.cpp"
        "src/wave_module.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app
        bsp
        model
        bmi270
        esp_lvgl_port
        lvgl
        vqf
        esp_timer
        heap
        mini_games
        wave_generator
        connectivity
)

if(M5_STICKS3_HW_SMOKE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_STICKS3_HW_SMOKE_TEST=1)
endif()
