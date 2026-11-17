idf_component_register(
    SRCS
        "src/motion_state.cpp" "src/motion_stream.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
)

