idf_component_register(
    SRCS
        "src/app_manager.cpp"
        "src/app_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES base freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
