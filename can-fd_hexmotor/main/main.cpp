// main.cpp - minimal C++ entry for ESP-IDF template
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

//user C++ library include
#include "logger.hpp"
#include "uart_dma_driver.hpp"
#include "canfd_driver.hpp"
#include "TaskReactor.hpp"
#include "canopen/canopen_sdo.hpp"
#include "motor/hexfellow_motor_task.hpp"

// cpp standard library
#include <atomic>
#include <charconv>
#include <string_view>

static const char * TAG         = "app_main";
static const std::string HEXMOTOR    = "Motor_task";

/**
 * @brief Atomic velocity setpoint — written by UartRxTask, read by app_main.
 * Task notification from UartRxTask to app_main provides the wake-up signal.
 */
static std::atomic<float> g_target_velocity{0.0f};
static TaskHandle_t       g_app_main_handle = nullptr;  ///< Set by app_main at startup

/********** global variable define beginning **********/
//freeRTOS task handle
TaskHandle_t Handle_ReactorFunc = nullptr;
TaskHandle_t Handle_MotorControlFunc = nullptr;
TaskHandle_t Handle_UartRxFunc = nullptr;       ///< Handle for the UART RX listener task
TaskReactor reactor(Handle_ReactorFunc);
/********** global variable define end       **********/

/**
 * @brief UART RX task — blocks on the native IDF UART event queue and
 *        parses incoming speed-adjustment commands.
 *
 * Supported commands:
 *   spd <velocity_rps>   — set target velocity (rev/s)
 *   spd ?                — query current setpoint
 *
 * Examples:
 *   spd 2.5     → motor 0 at 2.5 rev/s
 *   spd 0       → motor 0 stop
 *   spd -1.0    → motor 0 reverse at 1.0 rev/s
 *
 * @param pvParameters  Pointer to the Esp32UartDmaDriver instance.
 */
void UartRxTask(void* pvParameters) {
    auto* uart = static_cast<Esp32UartDmaDriver*>(pvParameters);
    uart_event_t event;
    espidf_template::Logger rx_log("UART_RX");

    while (true) {
        // Block until a UART event is posted by the IDF ISR.
        if (xQueueReceive(uart->event_queue(), &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA:
            // Drain available data as a null-terminated string and parse commands.
            uart->drainLines([&](const char* line) {
                rx_log.info("{}", line);

                // ── Split line into command + args ──────────────────
                TaskReactor::strCMD_t strCmd;
                if (!TaskReactor::parseStrCMD(std::string_view(line), strCmd)) {
                    return;  // empty or whitespace-only line
                }

                if (strCmd.command == "spd") {
                    // "spd ?" — query current setpoint
                    size_t qpos = strCmd.args.find_first_not_of(' ');
                    if (qpos != std::string_view::npos && strCmd.args[qpos] == '?') {
                        rx_log.info("[CMD] spd query → vel={:.2f} rps",
                                    g_target_velocity.load(std::memory_order_relaxed));
                        return;
                    }

                    // Parse velocity (float, rev/s)
                    float vel = 0.0f;
                    if (!TaskReactor::parseStrArg(strCmd.args, vel)) {
                        rx_log.warn("[CMD] bad velocity in: {}", line);
                        return;
                    }

                    // Publish to shared state and wake app_main
                    g_target_velocity.store(vel, std::memory_order_relaxed);
                    xTaskNotifyGive(g_app_main_handle);
                    rx_log.info("[CMD] set vel={:.2f} rps", vel);
                }
                // ──────────────────────────────────────────────────
            });
            break;

        case UART_FIFO_OVF:
            // Hardware RX FIFO overflow — flush and warn.
            ESP_LOGW(TAG, "UART RX FIFO overflow");
            uart_flush_input(UART_NUM_0);
            break;

        case UART_BUFFER_FULL:
            // Software RX ring buffer exhausted.
            ESP_LOGW(TAG, "UART RX buffer full");
            break;

        case UART_BREAK:
            ESP_LOGI(TAG, "UART break signal");
            break;

        default:
            break;
        }
    }
}

extern "C" void app_main(void)
{
    g_app_main_handle = xTaskGetCurrentTaskHandle();  // For UartRxTask notifications

    // ──────────── 1. Initialise DMA-UART (logger + command RX) ────────────
    Esp32UartDmaDriver::Config uart_cfg = {};
    uart_cfg.uart_num = UART_NUM_0;
    uart_cfg.tx_pin   = GPIO_NUM_11;          ///< TX — logging output
    uart_cfg.rx_pin   = GPIO_NUM_12;          ///< RX — incoming commands
    uart_cfg.baudrate = 115200;
    Esp32UartDmaDriver uart_drv(uart_cfg);

    if (!uart_drv.init() || !uart_drv.start()) {
        // Fallback: logging remains on the default UART0 console via ESP_LOGx.
        ESP_LOGE(TAG, "DMA-UART init failed, logging to default console");
    } else {
        // Route all Logger output and ESP_LOGx macros through DMA UART.
        espidf_template::Logger::setUart(&uart_drv);
        esp_log_set_vprintf(&espidf_template::Logger::vprintfHook);

        // Create the UART RX listener task (blocks on the IDF event queue).
        BaseType_t ret = xTaskCreate(UartRxTask,
                                     "uart_rx",
                                     2560,
                                     &uart_drv,
                                     10,
                                     &Handle_UartRxFunc);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create UART RX task");
        }
    }

    // ──────────── 2. Business logic ────────────────────────
    espidf_template::Logger logger(TAG);
    logger.info("ESP-IDF running hexmotor control.");

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
    cfg.motors[0].torque_permille = 500;
    cfg.motors[0].kp_kd_torque_permille = 770;
    HexfellowMotorController::mit_mapping_default(cfg.motors[0].mapping);
    // 电机1: 速度模式
    cfg.motors[1].mode = HexfellowMotorController::HEXFELLOW_MODE_KIND_VELOCITY;
    cfg.motors[1].torque_permille = 500;   // 速度模式的最大扭矩限制
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
    hexmotor_task.setVelocityTarget(1, 0.2f, 200);
    if(!hexmotor_task.start()) {
        ESP_LOGE(TAG, "Motor task failed to start — aborting main loop");
        return;
    }

    // Wait for motor CANopen SDO init to complete inside the motor task.
    // init_semaphore_ is given when controller_.init() succeeds in HexfellowMotorTask::main().
    if (!hexmotor_task.waitForInit(1500)) {
        ESP_LOGE(TAG, "Motor CANopen init failed or timed out — aborting main loop");
        ESP_LOGE(TAG,"now, aborting main loop");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    ESP_LOGI(TAG, "Motor CANopen init complete — entering main loop");

    /*────────────  Init GPIO  begin ────────────*/
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_25); // 
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    /*────────────  Init GPIO  end ────────────*/

    float motor0_velocity = 0.8f;           // 当前目标速度 (Rev/s)
    constexpr float kMaxVelocity = 3.0f;   // 最高速度 (Rev/s)

    while (true) {
        static int LED_State = 0;
        LED_State = !LED_State;
        gpio_set_level(GPIO_NUM_25, LED_State); // Toggle GPIO25 to show activity

        // ── Wait for UART speed command or timeout (500ms) ──
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))) {
            // New command arrived — apply with clamping
            float new_vel = g_target_velocity.load(std::memory_order_relaxed);
            if (new_vel >  kMaxVelocity) new_vel =  kMaxVelocity;
            if (new_vel < -kMaxVelocity) new_vel = -kMaxVelocity;
            motor0_velocity = new_vel;
            espidf_template::Logger cmd_log("serial_cmd");
            cmd_log.info("applied: vel={:.2f} rps", motor0_velocity);
        }
        // ──────────────────────────────────────────────────

        // 更新0号电机MIT目标速度
        HexfellowMotorController::mit_target_t target{};
        target.position = 0.0f;
        target.velocity = motor0_velocity;
        target.kp = 0.0f;
        target.kd = 0.6f;
        target.torque = 0.0f;
        hexmotor_task.setMitTarget(0, target);
        
        HexfellowMotorController::MotorState state;
        hexmotor_task.snapshot(0, state);

        // std::format-style logger (direct API, type-safe)
        espidf_template::Logger motor_log("motor[0]");
        motor_log.info("target_vel={:.1f} rps, pos={:.3f} rev, torque={} ‰, temp={}.{}°C",
            motor0_velocity,
            state.position_rev,
            state.raw_torque_permille,
            state.motor_temp_x10 / 10, state.motor_temp_x10 % 10);
    }
}
