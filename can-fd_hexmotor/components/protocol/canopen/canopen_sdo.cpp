#include "canopen_sdo.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include <cstring>


#define SDO_CLI_DL_INITIATE_EXP(n)  (0x23u | (((4u - (uint8_t)(n)) & 0x3u) << 2))
#define SDO_CLI_UL_INITIATE         (0x40u)
#define SDO_SRV_DL_RESPONSE         (0x60u)
#define SDO_SRV_UL_RESPONSE_EXP     (0x40u)
#define SDO_ABORT                   (0x80u)

void co_master_sdo::drain_rx_buffer() {
    can_driver_.signal_RxComplete([](bsp::canfd::Frame&) {});
    
    // 清除当前任务上可能残留的该位通知
    uint32_t cleared_bits = 0;
    xTaskNotifyWait(notification_bit_, 0, &cleared_bits, 0);
}

bool co_master_sdo::send_sdo_frame(uint8_t node, const uint8_t cmd[8])
{
    bsp::canfd::Frame frame{};
    frame.id = (COB_RSDO | node) & 0x7FFu;
    frame.extended = false;
    frame.fd_format = false;        // SDO: classic CAN frame
    frame.bitrate_switch = false;
    frame.dlc = 8;
    std::memcpy(frame.data.data(), cmd, 8);

    return can_driver_.send(frame);
}

co_master_sdo::SdoResult co_master_sdo::wait_for_sdo_response(uint8_t node, uint16_t idx, uint8_t sub,uint8_t out_frame[8], int timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    
    bool matched = false;
    SdoResult status = SdoResult::ERR_TIMEOUT;

    while (!matched) {
        TickType_t current_tick = xTaskGetTickCount();
        TickType_t elapsed = current_tick - start_tick;
        
        if (elapsed >= timeout_ticks) {
            return SdoResult::ERR_TIMEOUT;
        }
        TickType_t remaining_ticks = timeout_ticks - elapsed;

        uint32_t notified_value = 0;
        // 阻塞等待底层 ISR 触发 Reactor 事件通知
        if (xTaskNotifyWait(0, notification_bit_, &notified_value, remaining_ticks) == pdTRUE) {
            
            // 调度驱动处理收到的帧，利用 C++ Lambda 闭包捕获进行匹配验证
            can_driver_.signal_RxComplete([&](bsp::canfd::Frame& frame) {
                if (matched) return; // 已经匹配到正确帧则跳过后续数据
                
                // 校验：必须是经典标帧、且 DLC 满足标准 CANopen 要求
                if (frame.extended || frame.dlc < 8) return;

                uint16_t target_cob_id = COB_TSDO | node;
                if (frame.id != target_cob_id) return;

                // 提取 Index 和 SubIndex
                uint16_t resp_idx = static_cast<uint16_t>(frame.data[1]) | (static_cast<uint16_t>(frame.data[2]) << 8);
                uint8_t  resp_sub = frame.data[3];

                if (resp_idx == idx && resp_sub == sub) {
                    std::memcpy(out_frame, frame.data.data(), 8);
                    matched = true;
                    status = SdoResult::OK;
                }
            });
        } else {
            return SdoResult::ERR_TIMEOUT;
        }
    }
    return status;
}



int co_master_sdo::download(uint8_t node, uint16_t idx, uint8_t sub, const void* data, uint8_t len,int timeout_ms, uint32_t* abort_code)
{
    if (!data || len == 0 || len > 4) return static_cast<int>(SdoResult::ERR_ARG);

    drain_rx_buffer();

    uint8_t cmd[8] = {0};
    cmd[0] = SDO_CLI_DL_INITIATE_EXP(len);
    cmd[1] = static_cast<uint8_t>(idx & 0xFF);
    cmd[2] = static_cast<uint8_t>((idx >> 8) & 0xFF);
    cmd[3] = sub;
    std::memcpy(&cmd[4], data, len);

    if (!send_sdo_frame(node, cmd)) {
        return static_cast<int>(SdoResult::ERR_IO);
    }

    uint8_t resp[8];
    SdoResult rc = wait_for_sdo_response(node, idx, sub, resp, timeout_ms);
    if (rc != SdoResult::OK) return static_cast<int>(rc);

    // 解析中止帧 (Abort)
    if ((resp[0] & 0xE0) == SDO_ABORT) {
        if (abort_code) {
            *abort_code = static_cast<uint32_t>(resp[4]) | (static_cast<uint32_t>(resp[5]) << 8) |
                          static_cast<uint32_t>(resp[6] << 16) | (static_cast<uint32_t>(resp[7]) << 24);
        }
        return static_cast<int>(SdoResult::ERR_ABORT);
    }

    if (resp[0] != SDO_SRV_DL_RESPONSE) return static_cast<int>(SdoResult::ERR_PROTOCOL);
    return static_cast<int>(SdoResult::OK);
}

int co_master_sdo::upload(uint8_t node, uint16_t idx, uint8_t sub, void* out, uint8_t* out_len, int timeout_ms, uint32_t* abort_code)
{
    if (!out || !out_len) return static_cast<int>(SdoResult::ERR_ARG);

    drain_rx_buffer();

    uint8_t cmd[8] = {0};
    cmd[0] = SDO_CLI_UL_INITIATE;
    cmd[1] = static_cast<uint8_t>(idx & 0xFF);
    cmd[2] = static_cast<uint8_t>((idx >> 8) & 0xFF);
    cmd[3] = sub;

    if (!send_sdo_frame(node, cmd)) {
        return static_cast<int>(SdoResult::ERR_IO);
    }

    uint8_t resp[8];
    SdoResult rc = wait_for_sdo_response(node, idx, sub, resp, timeout_ms);
    if (rc != SdoResult::OK) return static_cast<int>(rc);

    // 解析中止帧 (Abort)
    if ((resp[0] & 0xE0) == SDO_ABORT) {
        if (abort_code) {
            *abort_code = static_cast<uint32_t>(resp[4]) | (static_cast<uint32_t>(resp[5]) << 8) |
                          static_cast<uint32_t>(resp[6] << 16) | (static_cast<uint32_t>(resp[7]) << 24);
        }
        return static_cast<int>(SdoResult::ERR_ABORT);
    }

    // 校验是否为合法的 Expedited 响应数据 (SCS=2)
    if ((resp[0] & 0xE0) != SDO_SRV_UL_RESPONSE_EXP) return static_cast<int>(SdoResult::ERR_PROTOCOL);
    if ((resp[0] & 0x02) == 0) return static_cast<int>(SdoResult::ERR_PROTOCOL); // 暂不支持分段传输(Segmented)

    uint8_t n_unused = (resp[0] >> 2) & 0x03;
    uint8_t data_len = ((resp[0] & 0x01) ? (4 - n_unused) : 4);
    if (data_len > 4) return static_cast<int>(SdoResult::ERR_PROTOCOL);

    std::memcpy(out, &resp[4], data_len);
    *out_len = data_len;
    return static_cast<int>(SdoResult::OK);
}

int co_master_sdo::dl_u8(uint8_t n, uint16_t i, uint8_t sub, uint8_t v, int timeout_ms)
{
    return download(n, i, sub, &v, 1, timeout_ms);
}

int co_master_sdo::dl_u16(uint8_t n, uint16_t i, uint8_t sub, uint16_t v, int timeout_ms)
{
    uint8_t buf[2] = { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF) };
    return download(n, i, sub, buf, 2, timeout_ms);
}

int co_master_sdo::dl_u32(uint8_t n, uint16_t i, uint8_t sub, uint32_t v, int timeout_ms)
{
    uint8_t buf[4] = {
        static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)
    };
    return download(n, i, sub, buf, 4, timeout_ms);
}

int co_master_sdo::dl_i32(uint8_t n, uint16_t i, uint8_t sub, int32_t v, int timeout_ms)
{
    uint8_t buf[4] = {
        static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)
    };
    return download(n, i, sub, buf, 4, timeout_ms);
}

int co_master_sdo::dl_i8(uint8_t n, uint16_t i, uint8_t sub, int8_t v, int timeout_ms) {
    return download(n, i, sub, &v, 1, timeout_ms);
}

int co_master_sdo::dl_f32(uint8_t n, uint16_t i, uint8_t sub, float v, int timeout_ms) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return dl_u32(n, i, sub, bits, timeout_ms);
}

int co_master_sdo::ul_u8(uint8_t n, uint16_t i, uint8_t sub, uint8_t* v, int timeout_ms){
    uint8_t buf[4] = {0}, got = 0;
    int rc = upload(n, i, sub, buf, &got, timeout_ms);
    if (rc != static_cast<int>(SdoResult::OK)) return rc;
    if (got < 1) return static_cast<int>(SdoResult::ERR_PROTOCOL);
    *v = buf[0];
    return static_cast<int>(SdoResult::OK);
}

int co_master_sdo::ul_u16(uint8_t n, uint16_t i, uint8_t sub, uint16_t* v, int timeout_ms){
    uint8_t buf[4] = {0}, got = 0;
    int rc = upload(n, i, sub, buf, &got, timeout_ms);
    if (rc != static_cast<int>(SdoResult::OK)) return rc;
    if (got < 2) return static_cast<int>(SdoResult::ERR_PROTOCOL);
    *v = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
    return static_cast<int>(SdoResult::OK);
}

int co_master_sdo::ul_u32(uint8_t n, uint16_t i, uint8_t sub, uint32_t* v, int timeout_ms){
    uint8_t buf[4] = {0}, got = 0;
    int rc = upload(n, i, sub, buf, &got, timeout_ms);
    if (rc != static_cast<int>(SdoResult::OK)) return rc;
    if (got < 4) return static_cast<int>(SdoResult::ERR_PROTOCOL);
    *v = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
    return static_cast<int>(SdoResult::OK);
}

int co_master_sdo::ul_f32(uint8_t n, uint16_t i, uint8_t sub, float* v, int timeout_ms)
{
    uint32_t bits = 0;
    int rc = ul_u32(n, i, sub, &bits, timeout_ms);
    if (rc != static_cast<int>(SdoResult::OK)) return rc;
    std::memcpy(v, &bits, 4);
    return static_cast<int>(SdoResult::OK);
}

const char* co_master_sdo::strerr(SdoResult rc) {
    switch (rc) {
        case SdoResult::OK: return "OK";
        case SdoResult::ERR_IO: return "I/O error";
        case SdoResult::ERR_TIMEOUT: return "Timeout";
        case SdoResult::ERR_ABORT: return "Aborted by server";
        case SdoResult::ERR_PROTOCOL: return "Protocol error";
        case SdoResult::ERR_ARG: return "Invalid argument";
        default: return "Unknown error";
    }
}
