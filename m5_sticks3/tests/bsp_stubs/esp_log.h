#pragma once
inline void bsp_test_log(const char*, const char*, ...) {}
#define ESP_LOGE(...) bsp_test_log(__VA_ARGS__)
#define ESP_LOGI(...) bsp_test_log(__VA_ARGS__)
#define ESP_LOGW(...) bsp_test_log(__VA_ARGS__)
