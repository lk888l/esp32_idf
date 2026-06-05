// main.cpp - minimal C++ entry for ESP-IDF template
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "logger.hpp"
#include "canfd_driver.hpp"

static const char *TAG = "app_main";

extern "C" void app_main(void)
{
    espidf_template::Logger logger("app_main");
    logger.info("ESP-IDF C++ template running");

    Esp32CanFdDriver::Config can_cfg = {};
    can_cfg.tx_pin = GPIO_NUM_4;
    can_cfg.rx_pin = GPIO_NUM_5;
    can_cfg.arbitration_bitrate = 1000000;
    can_cfg.data_bitrate = 5000000;

    Esp32CanFdDriver can_driver(can_cfg);

    if(!can_driver.init()) {
        logger.info("Failed to initialize CAN driver");
        return;
    }

    if(!can_driver.start()) {
        logger.info("Failed to start CAN driver");
        return;
    }



    while (true) {
        uint8_t data[8] = {
            0x2F,
            0x60,
            0x60,
            0x00,
            0x03,
            0x00,
            0x00,
            0x00
        };

        bsp::canfd::Frame frame;
        frame.id = 0x62F;
        frame.extended = false;
        frame.fd_format = true;
        frame.bitrate_switch = true;
        frame.dlc = 8;
        memcpy(frame.data.data(), data, 8);

        can_driver.send(frame);

        logger.info("Heartbeat");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
