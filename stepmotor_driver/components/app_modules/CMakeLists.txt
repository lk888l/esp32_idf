idf_component_register(
    SRCS "src/foc_demo_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app bsp hardware logger motor_control freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
