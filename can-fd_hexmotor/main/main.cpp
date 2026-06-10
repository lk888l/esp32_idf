// main.cpp - minimal C++ entry for ESP-IDF template
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

//user C++ library include
#include "logger.hpp"
#include "canfd_driver.hpp"
#include "TaskReactor.hpp"
#include "canopen/canopen_sdo.hpp"

static const char *TAG = "app_main";
static const char * MOTOR_TAG = "Motor_task";

/********** global variable define beginning **********/
//freeRTOS task handle
TaskHandle_t Handle_ReactorFunc = nullptr;
TaskHandle_t Handle_MotorControlFunc = nullptr;
TaskReactor reactor(Handle_ReactorFunc);

// Hardware driver instances

/********** global variable define end       **********/


void ReactorFunc(void* pvParameters){
    
    reactor.taskLoop();
    vTaskDelete(NULL); // 防止意外退出导致 crash
}

/**
 * @brief Motor control task
 */
void Motor_Control_Task(void* pvParameters){
    // TODO: This way task random make a bitmask.
    constexpr uint32_t CAN_RX_NOTIFY_BIT = (1 << 2);
    // Initialize CAN driver 
    Esp32CanFdDriver::Config can_cfg = {};
    can_cfg.tx_pin = GPIO_NUM_4;
    can_cfg.rx_pin = GPIO_NUM_5;
    can_cfg.arbitration_bitrate = 1000000;
    can_cfg.data_bitrate = 5000000;
    Esp32CanFdDriver can_driver(can_cfg);
    if(!can_driver.init()) {
        ESP_LOGI(MOTOR_TAG, "Failed to initialize CAN driver");
        return;
    }
    if(!can_driver.start()) {
        ESP_LOGI(MOTOR_TAG, "Failed to start CAN driver");
        return;
    }

    // bind the CAN receive signal to the reactor with this task's handle and a unique bitmask.
    can_driver.bindReactor(&Esp32CanFdDriver::signal_RxComplete, xTaskGetCurrentTaskHandle(), CAN_RX_NOTIFY_BIT);
    // Create a CANopen SDO master instance using the CAN driver and the same notification bit for synchronization
    co_master_sdo sdo(can_driver, CAN_RX_NOTIFY_BIT);

    uint8_t target_node = 0x01; // 假设电机从机 Node_ID = 1

    int res = sdo.dl_i8(target_node, 0x6060, 0x00, 3);
    res |= sdo.dl_i32(target_node, 0x60FF, 0x00, 0, 100);
    res |= sdo.dl_u16(target_node, 0x6040, 0x00, 6, 100);
    res |= sdo.dl_u16(target_node, 0x6072, 0x00, 200, 100);
    res |= sdo.dl_u16(target_node, 0x6040, 0x00, 7, 100);
    // data_to_write[0] = 0x0f,data_to_write[1] = 0x00,data_to_write[2] = 0x00,data_to_write[3] = 0x00;
    res |= sdo.dl_u16(target_node, 0x6040, 0x00, 0x0f, 100);
    res |= sdo.dl_f32(target_node, 0x60FF, 0x00, 1.0f, 100);

    if (res != static_cast<int>(co_master_sdo::SdoResult::OK)) {
        ESP_LOGE(TAG, "Write Modes of operation failed: %s", sdo.strerr(static_cast<co_master_sdo::SdoResult>(res)));
    } else {
        ESP_LOGI(TAG, "Write Modes of operation successfully.");
    }
    while (true) {
        // ESP_LOGI(MOTOR_TAG, "Motor control task running");
        while(!can_driver.emply_rx_buffer()) {
            bsp::canfd::Frame frame;
            if(can_driver.pop_rx_buffer(frame)) {
                ESP_LOGI(MOTOR_TAG, "Received CAN frame in motor control task. ID: 0x%08X, DLC: %d", frame.id, frame.dlc);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // 模拟控制周期
    }
}

extern "C" void app_main(void)
{
    espidf_template::Logger logger(TAG);
    logger.info("ESP-IDF C++ template running");

    // Esp32CanFdDriver::Config can_cfg = {};
    // can_cfg.tx_pin = GPIO_NUM_4;
    // can_cfg.rx_pin = GPIO_NUM_5;
    // can_cfg.arbitration_bitrate = 1000000;
    // can_cfg.data_bitrate = 5000000;

    // Esp32CanFdDriver can_driver(can_cfg);

    // if(!can_driver.init()) {
    //     logger.info("Failed to initialize CAN driver");
    //     return;
    // }

    // if(!can_driver.start()) {
    //     logger.info("Failed to start CAN driver");
    //     return;
    // }

    // Create a task for the reactor function
    // BaseType_t xReturn = pdPASS;
    // xReturn = xTaskCreate((TaskFunction_t)ReactorFunc,
    //                     (const char*)"Reactor",
    //                     4096,
    //                     (void*)NULL,
    //                     (UBaseType_t)8,
    //                     (TaskHandle_t*)&Handle_ReactorFunc);
    // if (xReturn != pdPASS) {
    //     logger.info("Failed to create reactor task");
    //     return;
    // }
    // Create a task for the motor control function
    BaseType_t xReturn = pdPASS;
    xReturn = xTaskCreate((TaskFunction_t)Motor_Control_Task,
                        (const char*)"MotorControl",
                        8192,
                        (void*)NULL,
                        (UBaseType_t)8,
                        (TaskHandle_t*)&Handle_MotorControlFunc);
    if (xReturn != pdPASS) {
        logger.info("Failed to create motor control task");
        return;
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_25); // 
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // 稍等一下让任务启动完成
    // vTaskDelay(pdMS_TO_TICKS(10));
    // reactor.connect(&can_driver, &Esp32CanFdDriver::signal_RxComplete, [&logger](bsp::canfd::Frame& frame) {
    //     // This lambda will be called when a CAN message is received
    //     logger.info("CAN message received!");
    //     ESP_LOGI("can", "Frame ID: 0x%08X", frame.id);
    //     for(size_t i = 0; i < frame.dlc; ++i) {
    //         ESP_LOGI("can", "Data[%lu]: 0x%02X", (unsigned long)i, frame.data[i]);
    //     }
    //     printf("\n");
    // });
    while (true) {
        // uint8_t data[8] = {
        //     0x40,
        //     0x18,
        //     0x10,
        //     0x01,
        //     0x00,
        //     0x00,
        //     0x00,
        //     0x00
        // };

        // bsp::canfd::Frame frame;
        // frame.id = 0x601;
        // frame.extended = false;
        // frame.fd_format = false;
        // frame.bitrate_switch = true;
        // frame.dlc = 8;
        // memcpy(frame.data.data(), data, 8);

        // can_driver.send(frame);

        // logger.info("Heartbeat"); 

        static int LED_State = 0;
        LED_State = !LED_State;
        gpio_set_level(GPIO_NUM_25, LED_State); // Toggle GPIO25 to show activity
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
