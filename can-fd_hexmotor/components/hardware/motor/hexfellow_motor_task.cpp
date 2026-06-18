// hexfellow_motor_task.cpp
#include "hexfellow_motor_task.hpp"
#include "esp_log.h"

HexfellowMotorTask::HexfellowMotorTask(const std::string& name,
                                       uint32_t stack_size,
                                       UBaseType_t priority,
                                       Esp32CanFdDriver& driver,
                                       const HexfellowMotorController::Config& cfg,
                                       BaseType_t core)
        : AppTask(name, stack_size, priority, core),
        driver_(driver),
        controller_(cfg)
{
    sdo_.emplace(driver,SDO_CAN_RX_NOTIFY_BIT);
    init_semaphore_ = xSemaphoreCreateBinary();
    // Binary semaphore starts at 0; will be given when motor init completes.
}

bool HexfellowMotorTask::initMotors()
{

    initialized_ = controller_.init(*sdo_, driver_);
    return initialized_;
}

void HexfellowMotorTask::setMitTarget(uint8_t index, const HexfellowMotorController::mit_target_t& target)
{
    controller_.setMitTarget(index, target);
}

void HexfellowMotorTask::setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille)
{
    controller_.setVelocityTarget(index, target_rev_s, torque_permille);
}

void HexfellowMotorTask::snapshot(uint8_t index, HexfellowMotorController::MotorState& out) const
{
    controller_.snapshot(index, out);
}

void HexfellowMotorTask::main()
{
    /* Re-bind the CAN RX signal to THIS task's handle.
     * The original bindReactor() in app_main() pointed to app_main's task,
     * but all SDO operations run in HexfellowMotorTask's context.  Without
     * this re-bind, the ISR notifies the wrong task and every SDO times out.
     */
    driver_.bindReactor(&Esp32CanFdDriver::signal_RxComplete,
                        xTaskGetCurrentTaskHandle(),
                        sdo_->get_notification_bit());

    if (!initialized_) {
        if (!controller_.init(*sdo_, driver_)) {
            // init_semaphore_ stays 0 → waitForInit() in app_main will time out
            return;
        }
        initialized_ = true;
    }

    // Signal app_main that motor CANopen init completed successfully.
    xSemaphoreGive(init_semaphore_);

    const TickType_t hb_period_ticks = pdMS_TO_TICKS(HexfellowMotorController::MASTER_HB_PERIOD_MS);
    TickType_t hb_last = xTaskGetTickCount();

    /* Initialise the periodic wake-time anchor just before the loop.
     * vTaskDelayUntil internally sets *pxPreviousWakeTime on the first call
     * if it is stale, but providing the current tick count avoids an
     * immediate catch-up burst when init() took significant time. */
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(RPDO_PERIOD_MS);

    for (;;) {
        if (shouldExit()) {
            break;
        }

        /* --- Heartbeat (50 ms) --- */
        const TickType_t now = xTaskGetTickCount();
        if ((now - hb_last) >= hb_period_ticks) {
            bsp::canfd::Frame hb{};
            hb.id = HexfellowMotorController::COB_HB | HexfellowMotorController::MASTER_NODE_ID;
            hb.extended = false;
            hb.fd_format = false;
            hb.bitrate_switch = false;
            hb.dlc = 1;
            hb.data[0] = 0x05;   // Operational
            driver_.send(hb);
            hb_last = now;
        }

        /* --- RPDO --- */
        bsp::canfd::Frame rpdo{};
        controller_.buildRpdoFrame(rpdo);
        driver_.send(rpdo);

        /* --- Process received CAN frames --- */
        driver_.signal_RxComplete([this](bsp::canfd::Frame& frame) {
            controller_.handleRxFrame(frame);
        });

        vTaskDelayUntil(&last_wake, period_ticks);
    }
}

void HexfellowMotorTask::cleanup()
{
    if (init_semaphore_ != nullptr) {
        vSemaphoreDelete(init_semaphore_);
        init_semaphore_ = nullptr;
    }
}

bool HexfellowMotorTask::waitForInit(uint32_t timeout_ms) const
{
    if (init_semaphore_ == nullptr) {
        return false;
    }
    return xSemaphoreTake(init_semaphore_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}