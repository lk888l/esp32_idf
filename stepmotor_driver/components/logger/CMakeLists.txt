idf_component_register(INCLUDE_DIRS "include" REQUIRES log)

target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_17)
