// hexfellow_motor_task.cpp
#include "hexfellow_motor_task.hpp"

HexfellowMotorTask::HexfellowMotorTask(const std::string& name,
                                       uint32_t stack_size,
                                       UBaseType_t priority,
                                       co_master_sdo& sdo,
                                       Esp32CanFdDriver& driver,
                                       const HexfellowMotorController::Config& cfg,
                                       BaseType_t core)
    : AppTask(name, stack_size, priority, core),
      sdo_(sdo),
      driver_(driver),
      controller_(cfg)
{
}

bool HexfellowMotorTask::initMotors()
{
    initialized_ = controller_.init(sdo_, driver_);
    return initialized_;
}

void HexfellowMotorTask::setMitTarget(uint8_t index, const mit_target_t& target)
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
    if (!initialized_) {
        if (!controller_.init(sdo_, driver_)) {
            return;
        }
        initialized_ = true;
    }

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t hb_last = 0;

    for (;;) {
        if (shouldExit()) {
            break;
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - hb_last) >= pdMS_TO_TICKS(HEXFELLOW_MASTER_HB_PERIOD_MS)) {
            bsp::canfd::Frame hb{};
            hb.id = HEXFELLOW_COB_HB | HEXFELLOW_MASTER_NODE_ID;
            hb.extended = false;
            hb.fd_format = false;
            hb.bitrate_switch = false;
            hb.dlc = 1;
            hb.data[0] = 0x05;   // Operational
            driver_.send(hb);
            hb_last = now;
        }

        bsp::canfd::Frame rpdo{};
        controller_.buildRpdoFrame(rpdo);
        driver_.send(rpdo);

        driver_.signal_RxComplete([this](bsp::canfd::Frame& frame) {
            controller_.handleRxFrame(frame);
        });

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEXFELLOW_RPDO_PERIOD_MS));
    }
}