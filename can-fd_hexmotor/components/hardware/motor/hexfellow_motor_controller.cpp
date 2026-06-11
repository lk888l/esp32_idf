// hexfellow_motor_controller.cpp
#include "hexfellow_motor_controller.hpp"

#include <algorithm>

static constexpr uint8_t kTpdo1Len = 12;
static constexpr uint8_t kTpdo2Len = 8;

HexfellowMotorController::HexfellowMotorController(const Config& cfg)
    : set_(cfg)
{
    std::memset(&runtime_set_, 0, sizeof(runtime_set_));
    runtime_set_.mode = cfg.mode;
    runtime_set_.count = cfg.count;
    runtime_set_.torque_permille = cfg.torque_permille;
    runtime_set_.kp_kd_torque_permille = cfg.kp_kd_torque_permille;
    runtime_set_.mapping = cfg.mapping;
}

uint64_t HexfellowMotorController::nowMs()
{
    return static_cast<uint64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
}

int32_t HexfellowMotorController::updateMultiTurn(int32_t prev_turns, float prev_pos, float new_pos)
{
    float diff = new_pos - prev_pos;
    if (diff > 0.5f)  return prev_turns - 1;
    if (diff < -0.5f) return prev_turns + 1;
    return prev_turns;
}

bool HexfellowMotorController::sendNmt(Esp32CanFdDriver& driver, uint8_t cs, uint8_t node) const
{
    bsp::canfd::Frame frame{};
    frame.id = HEXFELLOW_COB_NMT;
    frame.extended = false;
    frame.fd_format = false;
    frame.bitrate_switch = false;
    frame.dlc = 2;
    frame.data[0] = cs;
    frame.data[1] = node;
    return driver.send(frame);
}

void HexfellowMotorController::configureTpdo1(co_master_sdo& sdo, uint8_t id) const
{
    const uint32_t disable = 0xC0000180u | id;
    sdo.dl_u32(id, 0x1800, 1, disable);
    sdo.dl_u8 (id, 0x1A00, 0, 0);
    sdo.dl_u32(id, 0x1A00, 1, 0x60640020u);
    sdo.dl_u32(id, 0x1A00, 2, 0x10130020u);
    sdo.dl_u32(id, 0x1A00, 3, 0x60770010u);
    sdo.dl_u32(id, 0x1A00, 4, 0x603F0010u);
    sdo.dl_u8 (id, 0x1A00, 0, 4);
    sdo.dl_u8 (id, 0x1800, 2, 255);
    sdo.dl_u16(id, 0x1800, 3, HEXFELLOW_TPDO1_INHIBIT_X100US);
    sdo.dl_u16(id, 0x1800, 5, HEXFELLOW_TPDO1_EVENT_MS);
    const uint32_t enable = 0x40000180u | id;
    sdo.dl_u32(id, 0x1800, 1, enable);
}

void HexfellowMotorController::configureTpdo2(co_master_sdo& sdo, uint8_t id) const
{
    const uint32_t disable = 0xC0000280u | id;
    sdo.dl_u32(id, 0x1801, 1, disable);
    sdo.dl_u8 (id, 0x1A01, 0, 0);
    sdo.dl_u32(id, 0x1A01, 1, 0x60410010u);
    sdo.dl_u32(id, 0x1A01, 2, 0x22040110u);
    sdo.dl_u32(id, 0x1A01, 3, 0x22040210u);
    sdo.dl_u32(id, 0x1A01, 4, 0x60400010u);
    sdo.dl_u32(id, 0x1A01, 5, 0x603F0010u);
    sdo.dl_u8 (id, 0x1A01, 0, 5);
    sdo.dl_u8 (id, 0x1801, 2, 255);
    sdo.dl_u16(id, 0x1801, 3, HEXFELLOW_TPDO2_INHIBIT_X100US);
    sdo.dl_u16(id, 0x1801, 5, HEXFELLOW_TPDO2_EVENT_MS);
    const uint32_t enable = 0x40000280u | id;
    sdo.dl_u32(id, 0x1801, 1, enable);
}

void HexfellowMotorController::rpdoDisable(co_master_sdo& sdo, uint8_t id) const
{
    const uint32_t disable = 0x80000000u | HEXFELLOW_RPDO_COB_ID;
    sdo.dl_u32(id, 0x1400, 1, disable);
    sdo.dl_u8 (id, 0x1400, 2, 255);
}

void HexfellowMotorController::rpdoEnable(co_master_sdo& sdo, uint8_t id) const
{
    sdo.dl_u32(id, 0x1400, 1, HEXFELLOW_RPDO_COB_ID);
}

static uint32_t od_entry(uint16_t index, uint8_t sub, uint8_t bitlen)
{
    return (static_cast<uint32_t>(index) << 16) |
           (static_cast<uint32_t>(sub) << 8) |
           bitlen;
}

static constexpr uint32_t PAD_U32 = od_entry(0x3000, 0x03, 0x20);
static constexpr uint32_t PAD_U16 = od_entry(0x3000, 0x02, 0x10);

void HexfellowMotorController::rpdoMapMit(co_master_sdo& sdo, uint8_t id, uint8_t index, uint8_t total) const
{
    sdo.dl_u8(id, 0x1600, 0, 0);

    uint8_t sub = 1;
    for (uint8_t i = 0; i < total; ++i) {
        if (i == index) {
            sdo.dl_u32(id, 0x1600, sub++, od_entry(0x2004, 0x02, 0x20));
            sdo.dl_u32(id, 0x1600, sub++, od_entry(0x2004, 0x03, 0x20));
        } else {
            sdo.dl_u32(id, 0x1600, sub++, PAD_U32);
            sdo.dl_u32(id, 0x1600, sub++, PAD_U32);
        }
    }
    sdo.dl_u8(id, 0x1600, 0, static_cast<uint8_t>(total * 2));
}

void HexfellowMotorController::rpdoMapVelocity(co_master_sdo& sdo, uint8_t id, uint8_t index, uint8_t total) const
{
    sdo.dl_u8(id, 0x1600, 0, 0);

    uint8_t sub = 1;
    for (uint8_t i = 0; i < total; ++i) {
        if (i == index) {
            sdo.dl_u32(id, 0x1600, sub++, od_entry(0x6072, 0x00, 0x10));
            sdo.dl_u32(id, 0x1600, sub++, od_entry(0x60FF, 0x00, 0x20));
        } else {
            sdo.dl_u32(id, 0x1600, sub++, PAD_U16);
            sdo.dl_u32(id, 0x1600, sub++, PAD_U32);
        }
    }
    sdo.dl_u8(id, 0x1600, 0, static_cast<uint8_t>(total * 2));
}

bool HexfellowMotorController::initOneMotor(co_master_sdo& sdo, Esp32CanFdDriver& driver, uint8_t index)
{
    const uint8_t id = runtime_set_.motors[index].node_id;

    if (!sendNmt(driver, HEXFELLOW_NMT_PRE_OPERATIONAL, id)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint32_t factory_uid = 0;
    if (sdo.ul_u32(id, 0x1018, 1, &factory_uid) != 0) return false;
    if (factory_uid != HEXFELLOW_FACTORY_UID) return false;

    uint32_t fw = 0;
    if (sdo.ul_u32(id, 0x1018, 3, &fw) != 0) return false;
    if (fw != HEXFELLOW_REQUIRED_VERSION) return false;
    runtime_set_.motors[index].firmware_version = fw;

    uint32_t serial = 0;
    if (sdo.ul_u32(id, 0x1018, 4, &serial) != 0) return false;
    runtime_set_.motors[index].serial_number = serial;

    float peak = 0.0f;
    if (sdo.ul_f32(id, 0x6076, 0, &peak) != 0) return false;
    runtime_set_.motors[index].peak_torque_mNm = peak;

    sdo.dl_u16(id, 0x6040, 0, 0x0000);
    vTaskDelay(pdMS_TO_TICKS(10));
    sdo.dl_u16(id, 0x6040, 0, 0x0080);
    vTaskDelay(pdMS_TO_TICKS(10));
    sdo.dl_u16(id, 0x6040, 0, 0x0080);
    vTaskDelay(pdMS_TO_TICKS(10));

    sdo.dl_u32(id, 0x1016, 1, 0);
    sdo.dl_u8(id, 0x2040, 0, 1);
    sdo.dl_u16(id, 0x6072, 0, runtime_set_.torque_permille);

    if (runtime_set_.mode == HEXFELLOW_MODE_KIND_MIT) {
        const auto& m = runtime_set_.mapping;
        sdo.dl_u16(id, 0x2004, 0x0E, runtime_set_.kp_kd_torque_permille);
        sdo.dl_f32(id, 0x2004, 0x04, m.position_min);
        sdo.dl_f32(id, 0x2004, 0x05, m.position_max);
        sdo.dl_f32(id, 0x2004, 0x06, m.velocity_min);
        sdo.dl_f32(id, 0x2004, 0x07, m.velocity_max);
        sdo.dl_f32(id, 0x2004, 0x08, m.kp_min);
        sdo.dl_f32(id, 0x2004, 0x09, m.kp_max);
        sdo.dl_f32(id, 0x2004, 0x0A, m.kd_min);
        sdo.dl_f32(id, 0x2004, 0x0B, m.kd_max);
        sdo.dl_f32(id, 0x2004, 0x0C, m.torque_min);
        sdo.dl_f32(id, 0x2004, 0x0D, m.torque_max);

        mit_target_t zero{};
        uint8_t bytes[8]{};
        hexfellow_mit_target_pack(&zero, &m, bytes);
        uint32_t lo = static_cast<uint32_t>(bytes[0]) |
                      (static_cast<uint32_t>(bytes[1]) << 8) |
                      (static_cast<uint32_t>(bytes[2]) << 16) |
                      (static_cast<uint32_t>(bytes[3]) << 24);
        uint32_t hi = static_cast<uint32_t>(bytes[4]) |
                      (static_cast<uint32_t>(bytes[5]) << 8) |
                      (static_cast<uint32_t>(bytes[6]) << 16) |
                      (static_cast<uint32_t>(bytes[7]) << 24);
        sdo.dl_u32(id, 0x2004, 0x02, lo);
        sdo.dl_u32(id, 0x2004, 0x03, hi);
        sdo.dl_u8(id, 0x2004, 0x01, 1);
    }

    configureTpdo1(sdo, id);
    configureTpdo2(sdo, id);

    rpdoDisable(sdo, id);
    if (runtime_set_.mode == HEXFELLOW_MODE_KIND_MIT) {
        rpdoMapMit(sdo, id, index, runtime_set_.count);
    } else {
        rpdoMapVelocity(sdo, id, index, runtime_set_.count);
    }
    rpdoEnable(sdo, id);

    const uint32_t hb = (static_cast<uint32_t>(HEXFELLOW_MASTER_NODE_ID) << 16) |
                        HEXFELLOW_MOTOR_HB_TIMEOUT_MS;
    sdo.dl_u32(id, 0x1016, 1, hb);

    const int8_t mode_value = (runtime_set_.mode == HEXFELLOW_MODE_KIND_MIT)
                                ? HEXFELLOW_MODE_MIT
                                : HEXFELLOW_MODE_PROFILE_VELOCITY;
    sdo.dl_i8(id, 0x6060, 0, mode_value);

    sdo.dl_u16(id, 0x6040, 0, 0x0006);
    vTaskDelay(pdMS_TO_TICKS(10));
    sdo.dl_u16(id, 0x6040, 0, 0x0007);
    vTaskDelay(pdMS_TO_TICKS(10));
    sdo.dl_u16(id, 0x6040, 0, 0x000F);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!sendNmt(driver, HEXFELLOW_NMT_OPERATIONAL, id)) {
        return false;
    }

    return true;
}

bool HexfellowMotorController::init(co_master_sdo& sdo, Esp32CanFdDriver& driver)
{
    if (runtime_set_.count == 0 || runtime_set_.count > HEXFELLOW_MAX_MOTORS) {
        return false;
    }

    for (uint8_t i = 0; i < runtime_set_.count; ++i) {
        runtime_set_.motors[i].node_id = static_cast<uint8_t>(i + 1);
    }

    if (!sendNmt(driver, HEXFELLOW_NMT_PRE_OPERATIONAL, 0)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    for (uint8_t i = 0; i < runtime_set_.count; ++i) {
        if (!initOneMotor(sdo, driver, i)) {
            return false;
        }
    }
    return true;
}

void HexfellowMotorController::setMitTarget(uint8_t index, const mit_target_t& target)
{
    if (index >= HEXFELLOW_MAX_MOTORS) return;
    std::lock_guard<std::mutex> lk(mutex_);
    mit_targets_[index] = target;
}

void HexfellowMotorController::setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille)
{
    if (index >= HEXFELLOW_MAX_MOTORS) return;
    std::lock_guard<std::mutex> lk(mutex_);
    vel_target_rev_s_[index] = target_rev_s;
    vel_torque_permille_[index] = torque_permille;
}

void HexfellowMotorController::snapshot(uint8_t index, MotorState& out) const
{
    if (index >= HEXFELLOW_MAX_MOTORS) {
        std::memset(&out, 0, sizeof(out));
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    out = state_[index];
}

HexfellowMotorController::MotorState HexfellowMotorController::snapshot(uint8_t index) const
{
    MotorState out{};
    snapshot(index, out);
    return out;
}

bool HexfellowMotorController::buildRpdoFrame(bsp::canfd::Frame& out) const
{
    std::lock_guard<std::mutex> lk(mutex_);

    out = {};
    out.id = HEXFELLOW_RPDO_COB_ID;
    out.extended = false;
    out.fd_format = true;
    out.bitrate_switch = true;

    const uint8_t cnt = runtime_set_.count;
    if (runtime_set_.mode == HEXFELLOW_MODE_KIND_MIT) {
        out.dlc = static_cast<uint8_t>(cnt * 8);
        for (uint8_t i = 0; i < cnt; ++i) {
            hexfellow_mit_target_pack(&mit_targets_[i], &runtime_set_.mapping,
                                      &out.data[i * 8]);
        }
    } else {
        out.dlc = static_cast<uint8_t>(cnt * 6);
        for (uint8_t i = 0; i < cnt; ++i) {
            uint8_t* p = &out.data[i * 6];
            const uint16_t torque = vel_torque_permille_[i];
            const float target = vel_target_rev_s_[i];
            p[0] = static_cast<uint8_t>(torque & 0xFF);
            p[1] = static_cast<uint8_t>((torque >> 8) & 0xFF);
            std::memcpy(&p[2], &target, sizeof(float));
        }
    }
    return true;
}

void HexfellowMotorController::handleRxFrame(bsp::canfd::Frame& frame)
{
    const uint16_t cob = static_cast<uint16_t>(frame.id & 0x7FFu);
    const uint8_t node = static_cast<uint8_t>(cob & 0x7Fu);
    if (node == 0 || node > runtime_set_.count) {
        return;
    }

    const uint8_t index = static_cast<uint8_t>(node - 1);
    const uint16_t base = static_cast<uint16_t>(cob & 0x780u);

    std::lock_guard<std::mutex> lk(mutex_);
    MotorState& s = state_[index];

    if (base == HEXFELLOW_COB_TPDO1 && frame.dlc >= kTpdo1Len) {
        float pos = 0.0f;
        std::memcpy(&pos, frame.data.data() + 0, 4);
        const uint32_t ts = static_cast<uint32_t>(frame.data[4]) |
                            (static_cast<uint32_t>(frame.data[5]) << 8) |
                            (static_cast<uint32_t>(frame.data[6]) << 16) |
                            (static_cast<uint32_t>(frame.data[7]) << 24);
        const int16_t torque_pm = static_cast<int16_t>(
            static_cast<uint16_t>(frame.data[8]) |
            (static_cast<uint16_t>(frame.data[9]) << 8));
        const uint16_t err = static_cast<uint16_t>(frame.data[10]) |
                             (static_cast<uint16_t>(frame.data[11]) << 8);

        s.multi_turns = updateMultiTurn(s.multi_turns, s.position_rev, pos);
        s.position_rev = pos;
        s.timestamp_us = ts;
        s.raw_torque_permille = torque_pm;
        s.error_code = err;
        s.last_tpdo1_ms = nowMs();
    }
    else if (base == HEXFELLOW_COB_TPDO2 && frame.dlc >= kTpdo2Len) {
        const uint16_t status = static_cast<uint16_t>(frame.data[0]) |
                                (static_cast<uint16_t>(frame.data[1]) << 8);
        const int16_t drv_t = static_cast<int16_t>(
            static_cast<uint16_t>(frame.data[2]) |
            (static_cast<uint16_t>(frame.data[3]) << 8));
        const int16_t mtr_t = static_cast<int16_t>(
            static_cast<uint16_t>(frame.data[4]) |
            (static_cast<uint16_t>(frame.data[5]) << 8));
        const uint16_t ctrl = static_cast<uint16_t>(frame.data[6]) |
                              (static_cast<uint16_t>(frame.data[7]) << 8);

        s.status_word = status;
        s.driver_temp_x10 = drv_t;
        s.motor_temp_x10 = mtr_t;
        s.control_word = ctrl;
        s.last_tpdo2_ms = nowMs();
    }
}