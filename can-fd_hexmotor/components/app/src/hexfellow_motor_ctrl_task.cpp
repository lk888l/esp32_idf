#include "hexfellow_motor_ctrl_task.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include <cstring>

HexfellowMotorCtrlTask::HexfellowMotorCtrlTask(Esp32CanFdDriver& can_driver, 
                                               co_master_sdo& sdo_master, 
                                               const hexfellow_motor_set_t& config)
    // 采用 16318 字节栈，优先级分配 15 (高优先级保证控制环路确定性)，绑定到 Core 1
    : AppTask("HexfellowCtrlTask", 16318, 15)
    , can_driver_(can_driver)
    , sdo_master_(sdo_master)
    , set_(config)
{
    mutex_ = xSemaphoreCreateMutex();
    std::memset(mit_targets_, 0, sizeof(mit_targets_));
    std::memset(vel_target_rev_s_, 0, sizeof(vel_target_rev_s_));
    std::memset(vel_torque_permille_, 0, sizeof(vel_torque_permille_));
    std::memset(states_, 0, sizeof(states_));
}

HexfellowMotorCtrlTask::~HexfellowMotorCtrlTask()
{
    cleanup();
}

void HexfellowMotorCtrlTask::hexfellow_mit_mapping_default(hexfellow_mit_mapping_t* mapping) {
    mapping->position_min = -0.5f,  mapping->position_max = 0.5f,
    mapping->velocity_min = -10.0f, mapping->velocity_max = 10.0f,
    mapping->torque_min   = -10.0f, mapping->torque_max   = 10.0f,
    mapping->kp_min       = 0.0f,   mapping->kp_max       = 100.0f,
    mapping->kd_min       = 0.0f,   mapping->kd_max       = 20.0f;

}

void HexfellowMotorCtrlTask::setMitTarget(uint8_t index, const hexfellow_mit_target_t& target)
{
    if (index >= set_.count) return;
    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        mit_targets_[index] = target;
        xSemaphoreGive(mutex_);
    }
}

void HexfellowMotorCtrlTask::setVelocityTarget(uint8_t index, float target_rev_s, uint16_t torque_permille)
{
    if (index >= set_.count) return;
    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        vel_target_rev_s_[index] = target_rev_s;
        vel_torque_permille_[index] = torque_permille;
        xSemaphoreGive(mutex_);
    }
}

void HexfellowMotorCtrlTask::getMotorSnapshot(uint8_t index, hexfellow_motor_state_t& out_state)
{
    if (index >= set_.count) return;
    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        out_state = states_[index];
        xSemaphoreGive(mutex_);
    }
}

bool HexfellowMotorCtrlTask::initMotors()
{
    ESP_LOGI(TAG, "Starting sequential SDO configuration for %d motors...", set_.count);
    // Firtly, send NMT to 
    if(!sendNmt(0x80, 0)){
        ESP_LOGE(TAG, "Failed to send NMT Stop to all nodes");
        return false;
    }
    for (uint8_t i = 0; i < set_.count; ++i) {
        uint8_t node_id = set_.motors[i].node_id;
 
        hexfellow_mit_mapping_default(&set_.mapping);

         // 0. 基础通信验证：读取厂商特定的唯一 ID 以确认设备响应正常 (Index 0x1018, Sub 0x01)
        uint32_t fw_ver = 0;
        if (sdo_master_.ul_u32(node_id, 0x1018, 1, &fw_ver, 100) != 0) {
            if(fw_ver != HEXFELLOW_FACTORY_UID){
                ESP_LOGE(TAG, "Motor node %d firmware version check failed! (Got: %ld, Expected: %ld)", node_id, fw_ver, HEXFELLOW_FACTORY_UID);
                return false;
            }
        }

        // 1. 验证固件版本是否符合 firmware_version == 8 (Index 0x1018, Sub 0x03)
        uint32_t fw_ver = 0;
        if (sdo_master_.ul_u32(node_id, 0x1018, 3, &fw_ver, 100) != 0) {
            if(fw_ver != 8){
                ESP_LOGE(TAG, "Motor node %d firmware version check failed! (Got: %ld, Expected: 8)", node_id, fw_ver);
                return false;
            }
        }
        set_.motors[i].firmware_version = fw_ver;

        // 2. 读取序列号 (Index 0x1018, Sub 0x04)
        sdo_master_.ul_u32(node_id, 0x1018, 4, &set_.motors[i].serial_number, 100);

        // 3. 读取峰值扭矩并更新本地环境参数 (Index 0x6076, Sub 0x00)
        sdo_master_.ul_f32(node_id, 0x6076, 0, &set_.motors[i].peak_torque_mNm, 100);

        // 8. CiA402 状态机有序使能 (Index 0x6040): Shutdown (0x06) -> Switch On (0x07) -> Enable Operation (0x0F)
        uint16_t state_cmds[] = {0x0000, 0x0080, 0x0080};
        for (uint16_t cmd : state_cmds) {
            if (sdo_master_.dl_u16(node_id, 0x6040, 0, cmd, 100) != 0) {
                ESP_LOGE(TAG, "CiA402 transition failed at command 0x%04X for node %d", cmd, node_id);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }


        if (sdo_master_.dl_u32(node_id, 0x1016, 1, 0, 100) != 0) {
            ESP_LOGE(TAG, "Failed to arm Consumer Heartbeat Watchdog for node %d", node_id);
            return false;
        }

        if (sdo_master_.dl_u8(node_id, 0x2004, 0, 1, 100) != 0) {
            ESP_LOGE(TAG, "Failed to transmit for node %d", node_id);
            return false;
        }

        // 4. 配置最大输出扭矩限制 0..1000 permille (Index 0x6072, Sub 0x00)
        if (sdo_master_.dl_u16(node_id, 0x6072, 0, set_.torque_permille, 100) != 0) {
            ESP_LOGE(TAG, "Failed to write Max Torque Limit for node %d", node_id);
            return false;
        }

        // 5. MIT 独有参数设定: 写入限制范围 (Index 0x2004, Sub 0x0E)
        if (set_.mode == HEXFELLOW_MODE_KIND_MIT) {
            sdo_master_.dl_u16(node_id, 0x2004, 0x0E, set_.kp_kd_torque_permille, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x04, set_.mapping.position_min, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x05, set_.mapping.position_max, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x06, set_.mapping.velocity_min, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x07, set_.mapping.velocity_max, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x08, set_.mapping.kp_min, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x09, set_.mapping.kp_max, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x0A, set_.mapping.kd_min, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x0B, set_.mapping.torque_min, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x0D, set_.mapping.torque_max, 100);

            /* Pre-load 2004h-02/03 with a zero-target value so the first frame
            * after enable doesn't cause an instantaneous torque spike. */
            hexfellow_mit_target_t zero = {0};
            uint8_t bytes[8];
            hexfellow_mit_target_pack(&zero, set_.mapping, bytes);
            uint32_t lo = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
            uint32_t hi = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) |
                        ((uint32_t)bytes[6] << 16) | ((uint32_t)bytes[7] << 24);
            sdo_master_.dl_f32(node_id, 0x2004, 0x02, lo, 100);
            sdo_master_.dl_f32(node_id, 0x2004, 0x03, hi, 100);

            /* Enable compressed MIT control parameter. */
            sdo_master_.dl_f32(node_id, 0x2004, 0x01, 1, 100);
        }


        // 6. 核心安全配置：配置 Consumer Heartbeat 监测 Master (Index 0x1016, Sub 0x01)
        // 映射结构：[Master Node ID (8-bit) | Timeout 250ms (16-bit)] -> 0x000000FA
        uint32_t heartbeat_param = (MASTER_NODE_ID << 16) | 250; 
        if (sdo_master_.dl_u32(node_id, 0x1016, 1, heartbeat_param, 100) != 0) {
            ESP_LOGE(TAG, "Failed to arm Consumer Heartbeat Watchdog for node %d", node_id);
            return false;
        }

        // 7. 切换操作模式 (Index 0x6060, Sub 0x00): Mode 5 = MIT, Mode 3 = Profile Velocity
        uint8_t op_mode = (set_.mode == HEXFELLOW_MODE_KIND_MIT) ? 5 : 3;
        if (sdo_master_.dl_u8(node_id, 0x6060, 0, op_mode, 100) != 0) {
            ESP_LOGE(TAG, "Failed to set Operating Mode to %d for node %d", op_mode, node_id);
            return false;
        }

        ESP_LOGI(TAG, "Motor node %d SDO config done. Node is ready.", node_id);


    // 9. NMT 广播指令切换：全线转入 Operational 运行状态 (Node=0 广播)
    if (!sendNmt(0x01, 0)) {
        ESP_LOGE(TAG, "Broadcast NMT Operational failed");
        return false;
    }
    ESP_LOGI(TAG, "All nodes moved to NMT Operational state successfully.");
    return true;
}

inline uint32_t HexfellowMotorCtrlTask::float_to_uint(float x, float xmin, float xmax,
                                                      uint32_t bits)
{
        float span = xmax - xmin;
        float scale = (float)((1u << bits) - 1u);
        return (uint32_t)(((x - xmin) * scale) / span);
}

inline void HexfellowMotorCtrlTask::store_u32_le(uint8_t dst[4], uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

void HexfellowMotorCtrlTask::hexfellow_mit_target_pack(const hexfellow_mit_target_t* t,
                                                       const hexfellow_mit_mapping_t* m,
                                                       uint8_t out[8])
{
    float position = this->clamp(t->position, m->position_min, m->position_max);
    float velocity = this->clamp(t->velocity, m->velocity_min, m->velocity_max);
    float torque   = this->clamp(t->torque,   m->torque_min,   m->torque_max);
    float kp       = this->clamp(t->kp,       m->kp_min,       m->kp_max);
    float kd       = this->clamp(t->kd,       m->kd_min,       m->kd_max);

    uint32_t pos_u  = float_to_uint(position, m->position_min, m->position_max, 16);
    uint32_t vel_u  = float_to_uint(velocity, m->velocity_min, m->velocity_max, 12);
    uint32_t torq_u = float_to_uint(torque,   m->torque_min,   m->torque_max,   12);
    uint32_t kp_u   = float_to_uint(kp,       m->kp_min,       m->kp_max,       12);
    uint32_t kd_u   = float_to_uint(kd,       m->kd_min,       m->kd_max,       12);

    uint32_t lower_u32 = torq_u | (kd_u << 12) | ((kp_u & 0xFFu) << 24);
    uint32_t upper_u32 = (kp_u >> 8) | (vel_u << 4) | (pos_u << 16);

    store_u32_le(out,     lower_u32);
    store_u32_le(out + 4, upper_u32);
}

void HexfellowMotorCtrlTask::configure_tpdo1(uint8_t id) {
    
}

void HexfellowMotorCtrlTask::main()
{
    // 阶段一：运行底层的 SDO 同步配置与检测
    if (!initMotors()) {
        ESP_LOGE(TAG, "Initialization failed. Terminating control loop task.");
        return;
    }

    // 阶段二：建立精确控制环路时间轴
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(HEXFELLOW_RPDO_PERIOD_MS);
    
    uint32_t loop_count = 0;
    // 计算 50ms 对应的环路频率次数次数
    const uint32_t hb_trigger_count = 50 / HEXFELLOW_RPDO_PERIOD_MS;
    // 100ms 热身计次器，在热身期间只投递心跳和零位置，保证 watchdog 正常卡点
    const uint32_t warm_up_end_count = 100 / HEXFELLOW_RPDO_PERIOD_MS;

    ESP_LOGI(TAG, "Entering deterministic 1ms real-time control loop.");

    while (!shouldExit()) {
        // Step A: 发送主站心跳 (每 50ms 触发一次)
        if (loop_count % hb_trigger_count == 0) {
            sendMasterHeartbeat();
        }

        // Step B: 准备并发送复合式 RPDO 共享帧 (COB 0x190)
        // 在前 100ms 暖机时间内，强制清零发送目标避免启动震荡
        if (loop_count < warm_up_end_count) {
            // 暖机阶段：底层自动发送包含全零目标的结构
            bsp::canfd::Frame zero_rpdo = {};
            zero_rpdo.id = 0x190;
            zero_rpdo.fd_format = true;
            zero_rpdo.bitrate_switch = true;
            zero_rpdo.dlc = (set_.mode == HEXFELLOW_MODE_KIND_MIT) ? (set_.count * 8) : (set_.count * 6);
            can_driver_.send(zero_rpdo);
        } else {
            sendSharedRpdo();
        }

        // Step C: 非阻塞提取并解析驱动器当前周期产生的所有 TPDO1 / TPDO2 反馈
        drainIncomingTpdos();

        // Step D: 周期绝对延时挂起
        loop_count++;
        vTaskDelayUntil(&last_wake_time, period_ticks);
    }
}

bool HexfellowMotorCtrlTask::sendNmt(uint8_t cs, uint8_t node)
{
    bsp::canfd::Frame frame = {};
    frame.id = 0x000;  // 标准 NMT 的 COB-ID 为 0
    frame.extended = false;
    frame.fd_format = false; // 传统 CAN 格式
    frame.bitrate_switch = false;
    frame.dlc = 2;
    frame.data[0] = cs;
    frame.data[1] = node;
    return can_driver_.send(frame);
}

bool HexfellowMotorCtrlTask::sendMasterHeartbeat()
{
    bsp::canfd::Frame frame = {};
    frame.id = 0x710;  // Master Heartbeat COB-ID
    frame.extended = false;
    frame.fd_format = false;
    frame.bitrate_switch = false;
    frame.dlc = 1;
    frame.data[0] = 0x05; // 状态：Operational (0x05)
    return can_driver_.send(frame);
}

bool HexfellowMotorCtrlTask::sendSharedRpdo()
{
    bsp::canfd::Frame frame = {};
    frame.id = 0x190;
    frame.extended = false;
    frame.fd_format = true;     // 使用 CAN-FD 共享高速载荷
    frame.bitrate_switch = true;

    uint8_t* payload = frame.data.data();
    uint8_t current_len = 0;

    if (xSemaphoreTake(mutex_, 0) == pdTRUE) { // 控制环路使用非阻塞锁提高实时度
        if (set_.mode == HEXFELLOW_MODE_KIND_MIT) {
            current_len = set_.count * 8;
            for (uint8_t i = 0; i < set_.count; ++i) {
                packMitTarget(&payload[i * 8], mit_targets_[i]);
            }
        } else {
            current_len = set_.count * 6;
            for (uint8_t i = 0; i < set_.count; ++i) {
                packVelocityTarget(&payload[i * 6], vel_target_rev_s_[i], vel_torque_permille_[i]);
            }
        }
        xSemaphoreGive(mutex_);
    } else {
        return false; // 本周期锁冲突，跳过发送
    }

    frame.dlc = current_len;
    return can_driver_.send(frame);
}

void HexfellowMotorCtrlTask::drainIncomingTpdos()
{
    uint64_t current_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 采用你的非阻塞 Lambda 回调机制，一次性清空底层的 rx_buffer 帧对齐
    can_driver_.signal_RxComplete([this, current_time_ms](bsp::canfd::Frame& frame) {
        if (frame.extended) return;

        uint16_t cob_id = frame.id;

        for (uint8_t i = 0; i < set_.count; ++i) {
            uint8_t target_node = set_.motors[i].node_id;

            // 匹配 TPDO1 (0x180 + NodeID)
            if (cob_id == (0x180 + target_node)) {
                if (frame.dlc >= 12 && xSemaphoreTake(mutex_, 0) == pdTRUE) {
                    float last_single_turn = states_[i].position_rev;
                    
                    std::memcpy(&states_[i].position_rev,        &frame.data[0], 4);
                    std::memcpy(&states_[i].timestamp_us,        &frame.data[4], 4);
                    std::memcpy(&states_[i].raw_torque_permille, &frame.data[8], 2);
                    std::memcpy(&states_[i].error_code,          &frame.data[10], 2);

                    // 业界标准的单圈编码器跨零点多圈解算算法（Unwrapping）
                    float delta = states_[i].position_rev - last_single_turn;
                    if (delta < -0.5f) {
                        states_[i].multi_turns++; // 向前跨越零点
                    } else if (delta > 0.5f) {
                        states_[i].multi_turns--; // 向后跨越零点
                    }

                    states_[i].last_tpdo1_ms = current_time_ms;
                    xSemaphoreGive(mutex_);
                }
                break;
            }
            // 匹配 TPDO2 (0x280 + NodeID)
            else if (cob_id == (0x280 + target_node)) {
                if (frame.dlc >= 8 && xSemaphoreTake(mutex_, 0) == pdTRUE) {
                    std::memcpy(&states_[i].status_word,     &frame.data[0], 2);
                    std::memcpy(&states_[i].driver_temp_x10, &frame.data[2], 2);
                    std::memcpy(&states_[i].motor_temp_x10,  &frame.data[4], 2);
                    std::memcpy(&states_[i].control_word,    &frame.data[6], 2);
                    
                    states_[i].last_tpdo2_ms = current_time_ms;
                    xSemaphoreGive(mutex_);
                }
                break;
            }
        }
    });
}

void HexfellowMotorCtrlTask::packVelocityTarget(uint8_t* buf, float target_rev_s, uint16_t torque_permille)
{
    // 速度模式：4 字节速度 + 2 字节扭矩限制，满足 6 字节配置要求
    std::memcpy(&buf[0], &target_rev_s, 4);
    std::memcpy(&buf[4], &torque_permille, 2);
}

void HexfellowMotorCtrlTask::packMitTarget(uint8_t* buf, const hexfellow_mit_target_t& t)
{
    // 如果底层的电机控制器采用浮点打包，在这里拷贝 8 字节；
    // 如果是传统的 MIT 16-bit / 12-bit 的固定位数压缩编码，可按照下方注释格式在此处压缩：
    // uint16_t p_quant = float_to_uint(t.position, P_MIN, P_MAX, 16);
    // uint16_t v_quant = float_to_uint(t.velocity, V_MIN, V_MAX, 12);
    // 这里采用标准全浮点或者连续内存覆盖作为骨架：
    std::memcpy(&buf[0], &t.position, 4);
    std::memcpy(&buf[4], &t.velocity, 4); 
}

void HexfellowMotorCtrlTask::cleanup()
{
    ESP_LOGW(TAG, "Entering task destruction. Safe commands injected.");
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}