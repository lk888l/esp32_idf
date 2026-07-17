idf_component_register(
    SRCS "mt6816/src/mt6816.cpp"
    INCLUDE_DIRS "mt6816/include"
    REQUIRES bsp
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
