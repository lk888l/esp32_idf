#pragma once

inline void test_esp_logw(const char*, const char*, ...) {}

#define ESP_LOGW(...) test_esp_logw(__VA_ARGS__)
