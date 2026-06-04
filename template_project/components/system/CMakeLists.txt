idf_component_register(
    SRCS 
        "system.c"
    INCLUDE_DIRS 
        "."
    PRIV_REQUIRES
        esp_system
        esp_common
        freertos
        esp_hw_support
        esp_timer
)

# Set component-specific compilation flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
