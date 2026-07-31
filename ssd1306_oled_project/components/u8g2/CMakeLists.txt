idf_component_register(
    SRC_DIRS
        "csrc"
    INCLUDE_DIRS
        "csrc"
)

# OledDisplay associates the C callback object with its owning C++ instance.
# This must be public so every translation unit sees the same u8x8_t layout.
target_compile_definitions(${COMPONENT_LIB} PUBLIC U8X8_WITH_USER_PTR)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-overlength-strings
)
