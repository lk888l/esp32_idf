// hexfellow_motor_controller.hpp
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <cstring>

// #include "hexfellow_motor_init.h"
// #include "hexfellow_mit_target.h"
#include "canfd_driver.hpp"
#include "canopen/canopen_sdo.hpp"

class HexfellowMotorController
{
public:
    /*----------------   hex motor static variable define   ----------------*/ \
    /* Master node ID for this controller. The motors are configured to monitor
    * heartbeats from this node. */
    constexpr static uint8_t MASTER_NODE_ID = 0x10u;
    /* Shared RPDO COB-ID used for one-to-many control. Equal to the master's
    * default TPDO1 COB-ID (0x180 + 0x10). */
    constexpr static uint16_t HEXFELLOW_RPDO_COB_ID = 0x190u;

    /* Maximum number of motors on a single CAN bus. CAN-FD frame carries
    * up to 64 bytes: 8 motors × 8 bytes/motor (fixed slot, any mode). */
    constexpr static uint8_t MAX_MOTORS = 8;

    /* Vendor / firmware checks (1018h identity object). */
    constexpr static uint32_t FACTORY_UID = 0x4859444Cu;  /* factory UID reported by hexfellow firmware */
    constexpr static uint8_t MIN_VERSION = 8u;           /* minimum required firmware version (v8+) */

    /* Heartbeat parameters. */
    constexpr static uint8_t MASTER_HB_PERIOD_MS  =  50u;
    constexpr static uint16_t MOTOR_HB_TIMEOUT_MS  =  250u;

    /* CiA301 default COB-IDs */
    constexpr static uint16_t COB_NMT = 0x000u;
    constexpr static uint16_t OB_TPDO1  =   0x180u;
    constexpr static uint16_t COB_TPDO2  =   0x280u;
    constexpr static uint16_t COB_RSD   =   0x600u;
    constexpr static uint16_t COB_TSDO   =   0x580u;
    constexpr static uint16_t COB_HB     =   0x700u;

    /* TPDO event-timer / inhibit values (in 0.1ms units for inhibit, ms for event). */
    constexpr static uint8_t TPDO1_INHIBIT_X100US = 5u;
    constexpr static uint8_t TPDO1_EVENT_MS=1u;
    constexpr static uint8_t TPDO2_INHIBIT_X100US = 190u;
    constexpr static uint8_t TPDO2_EVENT_MS=20u;

    /* NMT command bytes */
    constexpr static uint8_t NMT_OPERATIONAL = 0x01u;
    constexpr static uint8_t NMT_PRE_OPERATIONAL=0x80u;
    constexpr static uint8_t NMT_RESET_NODE = 0x81u;
    constexpr static uint8_t NMT_RESET_COMM = 0x82u;

    /* CiA402 modes-of-operation values */
    constexpr static uint8_t MODE_PROFILE_VELOCITY = 3;   /* 6060h = 3 (mode 3) */
    constexpr static uint8_t MODE_MIT        =       5;   /* 6060h = 5 (MIT mode) */

    /*----------------   hex motor static variable define   ----------------*/ 
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

    struct MotorConfig
    {
        mode_kind_t mode{HEXFELLOW_MODE_KIND_MIT};
        uint16_t torque_permille{0};           // 6072h
        uint16_t kp_kd_torque_permille{0};     // 2004h-0E, MIT only
        mit_mapping_t mapping{};               // MIT only
    };

    struct Config
    {
        uint8_t  count{0};                         // 1..MAX_MOTORS
        MotorConfig motors[MAX_MOTORS]{};           // per-motor config
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



    /* Per-motor runtime information (config copy + learned during init). */
    typedef struct {
        mode_kind_t mode;
        uint16_t    torque_permille;
        uint16_t    kp_kd_torque_permille;
        mit_mapping_t mapping;             /* MIT only */
        /* --- learned during initialization --- */
        uint8_t     node_id;               /* CANopen ID, 1..N */
        uint32_t    firmware_version;      /* from 1018h-03 */
        uint32_t    serial_number;         /* from 1018h-04 */
        float       peak_torque_mNm;       /* from 6076h (REAL32, mNm) */
    } per_motor_runtime_t;

    typedef struct {
        uint8_t count;
        per_motor_runtime_t motors[MAX_MOTORS];
    } hexfellow_motor_set_t;
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
    mode_kind_t mode(uint8_t index) const { return runtime_set_.motors[index].mode; }

    /**
     * @brief 
     */
    static void hexfellow_mit_target_pack(const mit_target_t *t, const mit_mapping_t *m, uint8_t out[8]);

    /**
     * @brief 
     */
    template <typename T, typename L, typename H>
    static constexpr typename std::common_type<T, L, H>::type 
    clamp(const T& value, const L& low, const H& high) {
        using CommonType = typename std::common_type<T, L, H>::type;
        return (value < low) ? static_cast<CommonType>(low) : 
            (high < value) ? static_cast<CommonType>(high) : 
            static_cast<CommonType>(value);
    }
    static void mit_mapping_default(mit_mapping_t &mit_map);
    static inline uint32_t float_to_uint(float x, float xmin, float xmax, uint32_t bits);
    static inline void store_u32_le(uint8_t dst[4], uint32_t v);

private:
    static uint64_t nowMs();
    static int32_t updateMultiTurn(int32_t prev_turns, float prev_pos, float new_pos);

    bool sendNmt(Esp32CanFdDriver& driver, uint8_t cs, uint8_t node) const;
    bool initOneMotor(co_master_sdo& sdo, Esp32CanFdDriver& driver, uint8_t index);

    void configureTpdo1(co_master_sdo& sdo, uint8_t node_id) const;
    void configureTpdo2(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoDisable(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoEnable(co_master_sdo& sdo, uint8_t node_id) const;
    void rpdoMapMotor(co_master_sdo& sdo, uint8_t node_id, uint8_t index, uint8_t total) const;

private:
    Config set_{};
    hexfellow_motor_set_t runtime_set_{};

    std::array<mit_target_t, MAX_MOTORS> mit_targets_{};
    std::array<float,    MAX_MOTORS> vel_target_rev_s_{};
    std::array<uint16_t, MAX_MOTORS> vel_torque_permille_{};
    std::array<MotorState, MAX_MOTORS> state_{};

    mutable std::mutex mutex_;
};