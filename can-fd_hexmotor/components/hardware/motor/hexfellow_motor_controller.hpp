// hexfellow_motor_controller.hpp
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <cstring>

// #include "hexfellow_motor_init.h"
// #include "hexfellow_mit_target.h"
#include "canfd_driver.hpp"
#include "canopen_sdo.hpp"

class HexfellowMotorController
{
public:

    /*----------------   hex motor configure type   ----------------*/ 
    typedef enum {
        HEXFELLOW_MODE_KIND_MIT      = 0,
        HEXFELLOW_MODE_KIND_VELOCITY = 1,
    } mode_kind_t;

    typedef struct {
        float position_min, position_max;     /* Rev */
        float velocity_min, velocity_max;     /* Rev/s */
        float torque_min, torque_max;         /* Nm */
        float kp_min, kp_max;                 /* Nm/Rev */
        float kd_min, kd_max;                 /* Nm*s/Rev */
    } mit_mapping_t;

    typedef struct {
        float position;     /* Rev */
        float velocity;     /* Rev/s */
        float torque;       /* Nm (feed-forward) */
        float kp;           /* Nm/Rev */
        float kd;           /* Nm*s/Rev */
    } mit_target_t;

    typedef struct {
        mode_kind_t mode;
        uint8_t               count;
        uint16_t              torque_permille;
        uint16_t              kp_kd_torque_permille;
        uint16_t              mapping_placeholder; 
        hexfellow_mit_mapping_t mapping;             /* MIT only */
        hexfellow_motor_runtime_t motors[HEXFELLOW_MAX_MOTORS];
    } hexfellow_motor_set_t;    

    struct Config
    {
        mode_kind_t mode{HEXFELLOW_MODE_KIND_MIT};
        uint8_t  count{0};                         // 1..HEXFELLOW_MAX_MOTORS
        uint16_t torque_permille{0};               // 6072h
        uint16_t kp_kd_torque_permille{0};         // 2004h-0E, MIT only
        mit_mapping_t mapping{};         // MIT only
    };

    struct MotorState
    {
        float    position_rev{0.0f};               // 0x6064
        int32_t  multi_turns{0};                   // 本地累积
        uint32_t timestamp_us{0};                  // 0x1013
        int16_t  raw_torque_permille{0};           // 0x6077
        uint16_t error_code{0};                    // 0x603F

        uint16_t status_word{0};                   // 0x6041
        int16_t  driver_temp_x10{0};               // 0.1 C
        int16_t  motor_temp_x10{0};                // 0.1 C
        uint16_t control_word{0};                  // 0x6040 echo

        uint64_t last_tpdo1_ms{0};
        uint64_t last_tpdo2_ms{0};
    };
    /*----------------   hex motor configure type   ----------------*/ 



    explicit HexfellowMotorController(const Config& cfg);

    bool init(co_master_sdo& sdo, Esp32CanFdDriver& driver);

    void setMitTarget(uint8_t index, const mit_target_t& target);
    void setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille);

    void snapshot(uint8_t index, MotorState& out) const;
    MotorState snapshot(uint8_t index) const;

    bool buildRpdoFrame(bsp::canfd::Frame& out) const;
    void handleRxFrame(bsp::canfd::Frame& frame);

    uint8_t count() const { return set_.count; }
    mode_kind_t mode() const { return set_.mode; }

private:
    static uint64_t nowMs();
    static int32_t updateMultiTurn(int32_t prev_turns, float prev_pos, float new_pos);

    bool sendNmt(Esp32CanFdDriver& driver, uint8_t cs, uint8_t node) const;
    bool initOneMotor(co_master_sdo& sdo, Esp32CanFdDriver& driver, uint8_t index);

    void configureTpdo1(co_master_sdo& sdo, uint8_t node_id) const;
    void configureTpdo2(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoDisable(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoEnable(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoMapMit(co_master_sdo& sdo, uint8_t node_id, uint8_t index, uint8_t total) const;
    void rpdoMapVelocity(co_master_sdo& sdo, uint8_t node_id, uint8_t index, uint8_t total) const;

private:
    Config set_{};
    hexfellow_motor_set_t runtime_set_{};

    std::array<mit_target_t, HEXFELLOW_MAX_MOTORS> mit_targets_{};
    std::array<float,    HEXFELLOW_MAX_MOTORS> vel_target_rev_s_{};
    std::array<uint16_t, HEXFELLOW_MAX_MOTORS> vel_torque_permille_{};
    std::array<MotorState, HEXFELLOW_MAX_MOTORS> state_{};

    mutable std::mutex mutex_;
};