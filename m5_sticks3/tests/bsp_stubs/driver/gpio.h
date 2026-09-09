#pragma once
#include <cstdint>
#include "esp_err.h"
enum gpio_num_t {
 GPIO_NUM_11=11, GPIO_NUM_12=12, GPIO_NUM_14=14, GPIO_NUM_15=15,
 GPIO_NUM_16=16, GPIO_NUM_17=17, GPIO_NUM_18=18, GPIO_NUM_21=21,
 GPIO_NUM_38=38, GPIO_NUM_39=39, GPIO_NUM_40=40, GPIO_NUM_41=41,
 GPIO_NUM_42=42, GPIO_NUM_45=45, GPIO_NUM_46=46, GPIO_NUM_47=47, GPIO_NUM_48=48
};
constexpr int GPIO_MODE_INPUT=0, GPIO_PULLUP_ENABLE=1;
constexpr int GPIO_PULLDOWN_DISABLE=0, GPIO_INTR_DISABLE=0;
struct gpio_config_t {
 uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type;
};
inline esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }
