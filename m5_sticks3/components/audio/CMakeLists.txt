idf_component_register(
    SRCS "src/audio_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bsp esp_driver_i2s esp_codec_dev esp_timer esp_psram freertos
)
