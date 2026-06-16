#ifndef CANOPEN_SDO_HPP
#define CANOPEN_SDO_HPP

#include "canfd_driver.hpp"

class co_master_sdo {


    public:
        co_master_sdo(ICanDriver& can_driver, uint32_t notifyBit) : can_driver_(can_driver), notification_bit_(notifyBit) {};
        ~co_master_sdo() = default;

        /**
         * @brief CANopen SDO 状态/错误码
         */
        enum class SdoResult : int32_t {
            OK           = 0,
            ERR_IO       = -1,
            ERR_TIMEOUT  = -2,
            ERR_ABORT    = -3,  // 服务器（从机）终止传输
            ERR_PROTOCOL = -4,  // 协议错误（不符合预期的响应）
            ERR_ARG      = -5   // 参数错误
        };

        /**
         * @brief 阻塞式快捷下载（写入）数据到指定从机节点
         */
        int download(uint8_t node, uint16_t idx, uint8_t sub, const void* data, uint8_t len, int timeout_ms, uint32_t* abort_code = nullptr);

        /**
         * @brief 阻塞式快捷上传（读取）数据从指定从机节点
         */
        int upload(uint8_t node, uint16_t idx, uint8_t sub, void* out, uint8_t* out_len, int timeout_ms, uint32_t* abort_code = nullptr);

        /* ---- 强类型高频调用辅助函数 ---- */
        int dl_u8 (uint8_t n, uint16_t i, uint8_t sub, uint8_t  v, int timeout_ms = 100);
        int dl_u16(uint8_t n, uint16_t i, uint8_t sub, uint16_t v, int timeout_ms = 100);
        int dl_u32(uint8_t n, uint16_t i, uint8_t sub, uint32_t v, int timeout_ms = 100);
        int dl_i32(uint8_t n, uint16_t i, uint8_t sub, int32_t v, int timeout_ms = 100);
        int dl_i8 (uint8_t n, uint16_t i, uint8_t sub, int8_t   v, int timeout_ms = 100);
        int dl_f32(uint8_t n, uint16_t i, uint8_t sub, float    v, int timeout_ms = 100);

        int ul_u8 (uint8_t n, uint16_t i, uint8_t sub, uint8_t  *v, int timeout_ms = 100);
        int ul_u16(uint8_t n, uint16_t i, uint8_t sub, uint16_t *v, int timeout_ms = 100);
        int ul_u32(uint8_t n, uint16_t i, uint8_t sub, uint32_t *v, int timeout_ms = 100);
        int ul_f32(uint8_t n, uint16_t i, uint8_t sub, float    *v, int timeout_ms = 100);

        /**
         * @brief 获取当前 SDO 客户端所使用的通知比特掩码
         */
        uint32_t get_notification_bit() const { return notification_bit_; }

        /**
         * @brief 错误码转字符串
         */
        const char* strerr(SdoResult rc);

        static void uint32_to_sdo_data(uint32_t val, uint8_t arr[4]) {
            arr[0] = (uint8_t)(val & 0xFF);         // 最低位字节 (LSB)
            arr[1] = (uint8_t)((val >> 8) & 0xFF);
            arr[2] = (uint8_t)((val >> 16) & 0xFF);
            arr[3] = (uint8_t)((val >> 24) & 0xFF);        // 最高位字节 (MSB)
        }

    private:
        ICanDriver& can_driver_;
        uint32_t    notification_bit_;
        // CANopen standard COB-IDs for SDO
        static constexpr uint16_t COB_RSDO = 0x600;  // Master -> Slave (RxSDO)
        static constexpr uint16_t COB_TSDO = 0x580;  // Slave -> Master (TxSDO)

        bool sendRequest(uint32_t cobId, const uint8_t data[8]);
        bool waitResponse(uint32_t expectedCobId, uint8_t data[8], uint32_t timeoutMs);
        void drain_rx_buffer();
        bool send_sdo_frame(uint8_t node, const uint8_t cmd[8]);
        SdoResult wait_for_sdo_response(uint8_t node, uint16_t idx, uint8_t sub, uint8_t out_frame[8], int timeout_ms);
};

#endif //CANOPEN_SDO_HPP