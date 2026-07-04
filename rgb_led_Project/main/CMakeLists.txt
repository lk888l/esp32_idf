idf_component_register(
    SRCS
        "main.cpp"
        
    INCLUDE_DIRS 
        "."
    REQUIRES
        logger
        bsp
        led_strip
        driver
)

