// hexfellow_motor_task.hpp
#pragma once

#include <string>
#include <optional>

#include "freertos/FreeRTOS.h" // 正确：核心头文件置顶
#include "freertos/task.h"     // 随后可以引入其他 FreeRTOS 组件
#include "freertos/semphr.h"
#include "app_task.hpp"
#include "hexfellow_motor_controller.hpp"

class HexfellowMotorTask : public AppTask
{
public:

    constexpr static uint8_t RPDO_PERIOD_MS = 1;                // 支持 1ms 高频控制环路
    constexpr static uint32_t SDO_CAN_RX_NOTIFY_BIT = (1 << 2);     // TODO: This way task random make a bitmask. 

    HexfellowMotorTask(const std::string& name,
                       uint32_t stack_size,
                       UBaseType_t priority,
                       Esp32CanFdDriver& driver,
                       const HexfellowMotorController::Config& cfg,
                       BaseType_t core = tskNO_AFFINITY);

    bool initMotors();

    /**
     * @brief Block until motor CANopen init completes (or timeout).
     * @param timeout_ms  Max wait in milliseconds.
     * @return true if motors initialized, false on timeout (init failed).
     */
    bool waitForInit(uint32_t timeout_ms) const;

    void setMitTarget(uint8_t index, const HexfellowMotorController::mit_target_t& target);
    void setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille);
    void snapshot(uint8_t index, HexfellowMotorController::MotorState& out) const;

protected:
    void main() override;
    void cleanup() override;

private:
    Esp32CanFdDriver& driver_;
    std::optional<co_master_sdo> sdo_;
    HexfellowMotorController controller_;
    bool initialized_{false};
    SemaphoreHandle_t init_semaphore_{nullptr};   ///< Given when motor CANopen init succeeds
};