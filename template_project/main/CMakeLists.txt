idf_component_register(
    SRCS 
        "main.cpp"
        "app_init.cpp"
        "app_tasks.cpp"
        "app_manager.cpp"
    INCLUDE_DIRS 
        "."
        "include"
    REQUIRES 
        logger
        system
        nvs_flash
    PRIV_REQUIRES
        esp_common
        esp_system
        freertos
)

# Set component-specific compilation flags for C++
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -std=c++17
    -fno-rtti
)

# Enable C++ exceptions and thread-safe statics for ESP32
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

# Component version
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY
    COMPILE_DEFINITIONS
    MAIN_COMPONENT_VERSION="1.0.0"
)
