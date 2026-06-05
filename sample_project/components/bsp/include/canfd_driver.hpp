#pragma once

/// cpp standard library headers
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

/// Forward declaration of ESP-IDF TWAI types
#include "canfd_frame.hpp"

/// Forward declaration of ESP-IDF TWAI types
#include "esp_twai.h"
#include "esp_twai_onchip.h"

class ICanDriver
{
public:
    virtual ~ICanDriver() = default;

    virtual bool init() = 0;

    virtual bool start() = 0;

    virtual bool stop() = 0;

    virtual bool send(const bsp::canfd::Frame& frame) = 0;

    virtual bool receive(bsp::canfd::Frame& frame,
                         uint32_t timeout_ms) = 0;
};

class Esp32CanFdDriver : public ICanDriver
{
public:

    struct Config
    {
        gpio_num_t tx_pin;
        gpio_num_t rx_pin;

        uint32_t arbitration_bitrate;
        uint32_t data_bitrate;
    };

    explicit Esp32CanFdDriver(
        const Config& cfg);

    ~Esp32CanFdDriver();

    bool init() override;

    bool start() override;

    bool stop() override;

    bool send(const bsp::canfd::Frame& frame) override;

    bool receive(bsp::canfd::Frame& frame,
                 uint32_t timeout_ms) override;

private:

    Config config_;
    twai_node_handle_t node_ = nullptr;
    static bool on_rx_done(twai_node_handle_t node,const twai_rx_done_event_data_t *edata,void *user_ctx);
    
    std::mutex rx_mutex_;
    std::queue<bsp::canfd::Frame> rx_queue_;
    static constexpr size_t RX_QUEUE_LIMIT = 64;
};

