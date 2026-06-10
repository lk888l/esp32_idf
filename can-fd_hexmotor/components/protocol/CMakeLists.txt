idf_component_register(
    SRCS "canopen/canopen_sdo.cpp"
    INCLUDE_DIRS 
        "."

    REQUIRES 
        logger
        bsp
)