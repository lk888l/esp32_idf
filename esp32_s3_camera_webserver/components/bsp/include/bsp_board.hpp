#pragma once

#include "driver/gpio.h"

namespace bsp {

struct CameraPins {
    gpio_num_t pwdn;
    gpio_num_t reset;
    gpio_num_t xclk;
    gpio_num_t siod;
    gpio_num_t sioc;
    gpio_num_t y9;
    gpio_num_t y8;
    gpio_num_t y7;
    gpio_num_t y6;
    gpio_num_t y5;
    gpio_num_t y4;
    gpio_num_t y3;
    gpio_num_t y2;
    gpio_num_t vsync;
    gpio_num_t href;
    gpio_num_t pclk;
};

inline constexpr CameraPins kCameraPins{
    .pwdn = GPIO_NUM_NC,
    .reset = GPIO_NUM_NC,
    .xclk = GPIO_NUM_15,
    .siod = GPIO_NUM_4,
    .sioc = GPIO_NUM_5,
    .y9 = GPIO_NUM_16,
    .y8 = GPIO_NUM_17,
    .y7 = GPIO_NUM_18,
    .y6 = GPIO_NUM_12,
    .y5 = GPIO_NUM_10,
    .y4 = GPIO_NUM_8,
    .y3 = GPIO_NUM_9,
    .y2 = GPIO_NUM_11,
    .vsync = GPIO_NUM_6,
    .href = GPIO_NUM_7,
    .pclk = GPIO_NUM_13,
};

} // namespace bsp
