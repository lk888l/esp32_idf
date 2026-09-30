idf_component_register(
    SRCS "src/debug_probe.cpp" "src/probe_usb.cpp" "vendor/DAP.c" "vendor/SW_DP.c" "vendor/JTAG_DP.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "vendor"
    REQUIRES app esp_timer esp_driver_gpio esp_hw_support esp_system lwip usb esp_tinyusb
)
# The reference engine compiles out unsupported SWO/UART branches.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable)
