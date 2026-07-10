idf_component_register(
    SRCS
        "src/vqf.cpp"
    INCLUDE_DIRS
        "include"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    VQF_SINGLE_PRECISION
)
