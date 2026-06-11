#pragma once

#include "app_task.hpp"
#include "canfd_driver.hpp"
#include "canopen_sdo.hpp"
#include "freertos/semphr.h"
#include <vector>

// 保持与 Linux 端完全兼容的传统 C 结构定义
#define HEXFELLOW_MAX_MOTORS 8
#define HEXFELLOW_RPDO_PERIOD_MS 1  // 支持 1ms 高频控制环路

typedef enum {
    HEXFELLOW_MODE_KIND_MIT      = 0,
    HEXFELLOW_MODE_KIND_VELOCITY = 1,
} hexfellow_mode_kind_t;

typedef struct {
    uint8_t  node_id;
    uint32_t firmware_version;
    uint32_t serial_number;
    float    peak_torque_mNm;
} hexfellow_motor_runtime_t;

typedef struct {
    hexfellow_mode_kind_t mode;
    uint8_t               count;
    uint16_t              torque_permille;
    uint16_t              kp_kd_torque_permille;
    uint16_t              mapping_placeholder; 
    hexfellow_motor_runtime_t motors[HEXFELLOW_MAX_MOTORS];
} hexfellow_motor_set_t;

typedef struct {
    float    position;
    float    velocity;
    float    torque;
    float    kp;
    float    kd;
} hexfellow_mit_target_t;

typedef struct {
    float    position_rev;        
    int32_t  multi_turns;         
    uint32_t timestamp_us;        
    int16_t  raw_torque_permille; 
    uint16_t error_code;          
    uint16_t status_word;         
    int16_t  driver_temp_x10;     
    int16_t  motor_temp_x10;      
    uint16_t control_word;        
    uint64_t last_tpdo1_ms;
    uint64_t last_tpdo2_ms;
} hexfellow_motor_state_t;

/**
 * @brief Hexfellow 电机控制核心 RTOS 任务
 */
class HexfellowMotorCtrlTask : public AppTask
{
public:

    constexpr static uint32_t HEXFELLOW_FACTORY_UID = 0x4859444Cu;  /* factory UID reported by hexfellow firmware */
    constexpr static uint8_t MASTER_NODE_ID = 0x10u;  /* factory UID reported by hexfellow firmware */

    HexfellowMotorCtrlTask(Esp32CanFdDriver& can_driver, 
                           co_master_sdo& sdo_master, 
                           const hexfellow_motor_set_t& config);
    virtual ~HexfellowMotorCtrlTask();

    // 提供给应用层修改控制目标的线程安全 API
    void setMitTarget(uint8_t index, const hexfellow_mit_target_t& target);
    void setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille);
    
    // 提供给上层读取状态快照的线程安全 API
    void getMotorSnapshot(uint8_t index, hexfellow_motor_state_t& out_state);

protected:
    void main() override;
    void cleanup() override;

private:
    bool initMotors();
    bool sendNmt(uint8_t cs, uint8_t node);
    bool sendMasterHeartbeat();
    bool sendSharedRpdo();
    void drainIncomingTpdos();

    // 针对 MIT 紧凑协议的序列化辅助函数
    void packMitTarget(uint8_t* buf, const hexfellow_mit_target_t& t);
    void packVelocityTarget(uint8_t* buf, float target_rev_s, uint16_t torque_permille);

private:
    static constexpr const char* TAG = "HexfellowCtrl";

    Esp32CanFdDriver& can_driver_;
    co_master_sdo&    sdo_master_;
    
    hexfellow_motor_set_t set_;
    
    // 线程安全互斥锁与共享状态缓存
    SemaphoreHandle_t       mutex_ = nullptr;
    hexfellow_mit_target_t  mit_targets_[HEXFELLOW_MAX_MOTORS];
    float                   vel_target_rev_s_[HEXFELLOW_MAX_MOTORS];
    uint16_t                vel_torque_permille_[HEXFELLOW_MAX_MOTORS];
    hexfellow_motor_state_t states_[HEXFELLOW_MAX_MOTORS];
};