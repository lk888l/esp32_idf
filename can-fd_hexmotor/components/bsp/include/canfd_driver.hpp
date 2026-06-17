#pragma once

/// cpp standard library headers
#include <atomic>
#include <memory>

/// Forward declaration of ESP-IDF TWAI types
#include "canfd_frame.hpp"
#include "atomic_array.hpp"

/// Forward declaration of ESP-IDF TWAI types
#include "esp_twai.h"
#include "esp_twai_onchip.h"



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

private:

    Config config_;
    twai_node_handle_t node_ = nullptr;
    static constexpr uint32_t RX_RING_SIZE = 32;
    atomic_array<bsp::canfd::Frame, RX_RING_SIZE> rx_buffer;
    //signal config define
    SignalContext RxReceive_cfg{};

public:

    explicit Esp32CanFdDriver(const Config& cfg);

    ~Esp32CanFdDriver();

    bool init() override;

    bool start() override;

    bool stop() override;

    bool send(const bsp::canfd::Frame& frame) override;

    /**
     * @brief bind a signal to the reactor
     * @attention Only using the environment of FreeRTOS.
     * @param signal
     */
    template<typename SignalPtr>
    bool bindReactor(SignalPtr signalFunc, TaskHandle_t task, uint32_t bitMask){
        if constexpr (std::is_same_v<SignalPtr, decltype(&Esp32CanFdDriver::signal_RxComplete)>) {
            if (signalFunc == &Esp32CanFdDriver::signal_RxComplete) {
                RxReceive_cfg.task_h = task;
                RxReceive_cfg.bitMask = bitMask;
                return true;
            }
        }
        return false;
    }

    /**
     *  @brief a signal of can reveive complete. 
     *  \n     if connect the reactor, the driver will emit this signal when receive a frame and push it to the rx_buffer.
     *  @attention Only using the environment of FreeRTOS.
     */
    void signal_RxComplete(std::function<void(bsp::canfd::Frame&)> slot);

    size_t size_rx_buffer() const;
    bool emply_rx_buffer() const; 
    bool pop_rx_buffer(bsp::canfd::Frame& frame);
    
private:
    static bool on_rx_done( twai_node_handle_t node, const twai_rx_done_event_data_t* edata, void* user_ctx);
    bool push_rx_buffer(const bsp::canfd::Frame& frame);
};

