idf_component_register(
    SRCS "logger.c"
    INCLUDE_DIRS "."
)

# Set component-specific compilation flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
