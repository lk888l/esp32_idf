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
#include "motor/hexfellow_motor_task.hpp"

static const char * TAG         = "app_main";
static const char * MOTOR_TAG   = "Motor_task";
static const std::string HEXMOTOR    = "Motor_task";

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

    /// define Esp32CanFdDriver
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
    
    /// define hexmotor_control task
    // Create a CANopen SDO master instance using the CAN driver and the same notification bit for synchronization
    HexfellowMotorController::Config cfg;
    cfg.count = 1;
    // 0号电机，默认MIT模式
    cfg.motors[0].mode = HexfellowMotorController::HEXFELLOW_MODE_KIND_MIT;
    cfg.motors[0].torque_permille = 300;
    cfg.motors[0].kp_kd_torque_permille = 770;
    HexfellowMotorController::mit_mapping_default(cfg.motors[0].mapping);
    // 电机1: 速度模式
    cfg.motors[1].mode = HexfellowMotorController::HEXFELLOW_MODE_KIND_VELOCITY;
    cfg.motors[1].torque_permille = 300;   // 速度模式的最大扭矩限制
    // Create an instance of the motor control task
    HexfellowMotorTask hexmotor_task(HEXMOTOR, 8192, 8, can_driver, cfg);
    // 设置运行目标
    // 设置电机0运行模式
    HexfellowMotorController::mit_target_t target{};
    target.position = 0.0f;
    target.velocity = 1.0f;
    target.kp = 0.0f;
    target.kd = 1.0f;
    target.torque = 0.0f;
    hexmotor_task.setMitTarget(0,target);
    // 设置电机1 速度目标（target_rev_s=5.0 rps, torque_permille=200）
    hexmotor_task.setVelocityTarget(1, 2.0f, 200);
    if(!hexmotor_task.start()) {ESP_LOGE(TAG, "Initialization hexmotor task fail.");}
    // Create a task for the reactor function
    // Create a task for the motor control function
    // BaseType_t xReturn = pdPASS;
    // xReturn = xTaskCreate((TaskFunction_t)Motor_Control_Task,
    //                     (const char*)"MotorControl",
    //                     8192,
    //                     (void*)NULL,
    //                     (UBaseType_t)8,
    //                     (TaskHandle_t*)&Handle_MotorControlFunc);
    // if (xReturn != pdPASS) {
    //     logger.info("Failed to create motor control task");
    //     return;
    // }



    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_25); // 
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    while (true) {
        static int LED_State = 0;
        LED_State = !LED_State;
        gpio_set_level(GPIO_NUM_25, LED_State); // Toggle GPIO25 to show activity

        HexfellowMotorController::MotorState state;
        hexmotor_task.snapshot(0, state);
        ESP_LOGI("motor", "pos=%.3f rev, torque=%d ‰, temp=%d.%d°C",
            state.position_rev,
            state.raw_torque_permille,
            state.motor_temp_x10 / 10, state.motor_temp_x10 % 10);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
