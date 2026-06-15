// hexfellow_motor_task.hpp
#pragma once

#include <string>

#include "app_task.hpp"
#include "hexfellow_motor_controller.hpp"

class HexfellowMotorTask : public AppTask
{
public:

    constexpr static uint8_t RPDO_PERIOD_MS = 1;    // 支持 1ms 高频控制环路

    HexfellowMotorTask(const std::string& name,
                       uint32_t stack_size,
                       UBaseType_t priority,
                       co_master_sdo& sdo,
                       Esp32CanFdDriver& driver,
                       const HexfellowMotorController::Config& cfg,
                       BaseType_t core = tskNO_AFFINITY);

    bool initMotors();
    void setMitTarget(uint8_t index, const HexfellowMotorController::mit_target_t& target);
    void setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille);
    void snapshot(uint8_t index, HexfellowMotorController::MotorState& out) const;

protected:
    void main() override;
    void cleanup() override {}

private:
    co_master_sdo& sdo_;
    Esp32CanFdDriver& driver_;
    HexfellowMotorController controller_;
    bool initialized_{false};
};