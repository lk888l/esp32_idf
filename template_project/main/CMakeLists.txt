idf_component_register(
    SRCS 
        "main.c"
        "app_init.c"
        "app_tasks.c"
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
        log
        freertos
)

# Set component-specific compilation flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Component version
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY
    COMPILE_DEFINITIONS
    MAIN_COMPONENT_VERSION="1.0.0"
)
