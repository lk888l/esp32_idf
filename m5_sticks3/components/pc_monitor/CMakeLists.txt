idf_component_register(
    SRCS "src/pc_monitor_protocol.cpp" "src/pc_monitor_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos lwip
)
