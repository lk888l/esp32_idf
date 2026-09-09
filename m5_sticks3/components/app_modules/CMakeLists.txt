idf_component_register(
    SRCS
        "src/board_module.cpp"
        "src/connectivity_module.cpp"
        "src/button_module.cpp"
        "src/motion_module.cpp"
        "src/ui_module.cpp"
        "src/radio_ui.cpp"
        "src/wave_module.cpp"
        "src/peripheral_module.cpp"
        "src/peripheral_ui.cpp"
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
        audio
        infrared
)

if(M5_STICKS3_HW_SMOKE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_STICKS3_HW_SMOKE_TEST=1)
endif()

if(M5_STICKS3_RADIO_SMOKE_TEST)
    if(M5_STICKS3_HW_SMOKE_TEST)
        message(FATAL_ERROR "Choose one independent UI smoke test at a time")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_STICKS3_RADIO_SMOKE_TEST=1)
endif()
