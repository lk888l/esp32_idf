#include "canfd_driver.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include <cstring>

Esp32CanFdDriver::
Esp32CanFdDriver(const Config& cfg)
    : config_(cfg)
{
}

Esp32CanFdDriver::~Esp32CanFdDriver()
{
    stop();
}

bool Esp32CanFdDriver::init()
{
    twai_onchip_node_config_t node_cfg = {};

    node_cfg.io_cfg.tx = config_.tx_pin;
    node_cfg.io_cfg.rx = config_.rx_pin;
    node_cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    node_cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;


    node_cfg.bit_timing.bitrate =
        config_.arbitration_bitrate;

    node_cfg.bit_timing.sp_permill = 800;

    node_cfg.data_timing.bitrate =
        config_.data_bitrate;

    node_cfg.data_timing.sp_permill = 750;

    node_cfg.fail_retry_cnt = 0;
    node_cfg.tx_queue_depth = 10;
    node_cfg.flags.enable_self_test = 0;

    esp_err_t err =
        twai_new_node_onchip(
            &node_cfg,
            &node_);

    if(err != ESP_OK)
    {
        ESP_LOGE("CANFD",
                 "twai_new_node_onchip failed: %s (%d)",
                 esp_err_to_name(err),
                 err);
        return false;
    }

    // Register event callbacks
    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = on_rx_done;
    ESP_ERROR_CHECK(
        twai_node_register_event_callbacks(
        node_,
        &cbs,
        this));

    return err == ESP_OK;
}

bool Esp32CanFdDriver::start()
{
    return twai_node_enable(node_) == ESP_OK;
}

bool Esp32CanFdDriver::stop()
{
    if(node_ == nullptr)
        return true;

    twai_node_disable(node_);

    twai_node_delete(node_);

    node_ = nullptr;

    return true;
}

bool Esp32CanFdDriver::send(
    const bsp::canfd::Frame& frame)
{
    twai_frame_t tx = {};

    tx.header.id = frame.id;

    tx.header.ide =
        frame.extended;

    tx.header.fdf =
        frame.fd_format;

    tx.header.brs =
        frame.bitrate_switch;

    tx.header.rtr = false;
        
    tx.buffer =
        const_cast<uint8_t*>(
            frame.data.data());

    tx.buffer_len =
        frame.dlc;

    esp_err_t err =
        twai_node_transmit(
            node_,
            &tx,
            pdMS_TO_TICKS(10));

    return err == ESP_OK;
}

bool Esp32CanFdDriver::push_rx_frame(
    const bsp::canfd::Frame& frame)
{
    auto head =
        rx_ring_.head.load(
            std::memory_order_relaxed);

    auto next =
        (head + 1) % RX_RING_SIZE;

    auto tail =
        rx_ring_.tail.load(
            std::memory_order_acquire);

    if(next == tail)
    {
        return false;
    }

    rx_ring_.buffer[head] = frame;

    rx_ring_.head.store(
        next,
        std::memory_order_release);

    return true;
}

bool Esp32CanFdDriver::pop_rx_frame(
    bsp::canfd::Frame& frame)
{
    auto tail =
        rx_ring_.tail.load(
            std::memory_order_relaxed);

    auto head =
        rx_ring_.head.load(
            std::memory_order_acquire);

    if(tail == head)
    {
        return false;
    }

    frame =
        rx_ring_.buffer[tail];

    rx_ring_.tail.store(
        (tail + 1) % RX_RING_SIZE,
        std::memory_order_release);

    return true;
}

bool Esp32CanFdDriver::on_rx_done(
    twai_node_handle_t node,
    const twai_rx_done_event_data_t*,
    void* user_ctx)
{
    auto* self =
        static_cast<Esp32CanFdDriver*>(
            user_ctx);

    uint8_t data[64];

    twai_frame_t rx = {};

    rx.buffer = data;
    rx.buffer_len = sizeof(data);

    if(twai_node_receive_from_isr(
            node,
            &rx) != ESP_OK)
    {
        return false;
    }

    bsp::canfd::Frame frame;

    frame.id =
        rx.header.id;

    frame.extended =
        rx.header.ide;

    frame.fd_format =
        rx.header.fdf;

    frame.bitrate_switch =
        rx.header.brs;

    frame.dlc =
        rx.buffer_len;

    memcpy(
        frame.data.data(),
        data,
        frame.dlc);

    self->push_rx_frame(frame);

    return false;
}