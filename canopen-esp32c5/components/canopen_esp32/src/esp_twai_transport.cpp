#include "canopen_esp32/esp_twai_transport.hpp"

#include "esp_err.h"
#include "esp_log.h"

#include <algorithm>
#include <cstring>

namespace canopen_esp32 {
namespace {
constexpr char kTag[] = "twai_fd";
}

EspTwaiTransport::~EspTwaiTransport()
{
    (void)stop();
}

bool EspTwaiTransport::initialize()
{
    if (node_ != nullptr || config_.tx_queue_depth == 0 ||
        config_.tx_queue_depth > kMaxTxSlots || config_.rx_queue_depth == 0 ||
        config_.rx_queue_depth > kMaxRxDepth) {
        return false;
    }

    free_slots_queue_ = xQueueCreateStatic(config_.tx_queue_depth,
                                           sizeof(uint8_t),
                                           free_slots_storage_.data(),
                                           &free_slots_queue_control_);
    rx_queue_ = xQueueCreateStatic(config_.rx_queue_depth,
                                   sizeof(can::Frame),
                                   rx_storage_.data(),
                                   &rx_queue_control_);
    if (free_slots_queue_ == nullptr || rx_queue_ == nullptr) {
        return false;
    }
    for (uint8_t slot = 0; slot < config_.tx_queue_depth; ++slot) {
        if (xQueueSend(free_slots_queue_, &slot, 0) != pdTRUE) {
            return false;
        }
    }

    twai_onchip_node_config_t node_config{};
    node_config.io_cfg.tx = config_.tx_pin;
    node_config.io_cfg.rx = config_.rx_pin;
    node_config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    node_config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    node_config.bit_timing.bitrate = 1'000'000;
    node_config.bit_timing.sp_permill = 800;
    node_config.data_timing.bitrate = 5'000'000;
    node_config.data_timing.sp_permill = 750;
    node_config.fail_retry_cnt = 3;
    node_config.tx_queue_depth = config_.tx_queue_depth;
    node_config.intr_priority = 2;

    esp_err_t error = twai_new_node_onchip(&node_config, &node_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "twai_new_node_onchip failed: %s", esp_err_to_name(error));
        node_ = nullptr;
        return false;
    }

    // 80 MHz controller clock. These fields match:
    // ip link set can0 type can bitrate 1000000 sample-point 0.8 sjw 5
    //     dbitrate 5000000 dsample-point 0.75 dsjw 3 fd on
    const twai_timing_advanced_config_t nominal{
        .clk_src = TWAI_CLK_SRC_DEFAULT,
        .quanta_resolution_hz = 0,
        .brp = 1,
        .prop_seg = 31,
        .tseg_1 = 32,
        .tseg_2 = 16,
        .sjw = 5,
        .ssp_offset = 0,
        .triple_sampling = false,
    };
    const twai_timing_advanced_config_t data{
        .clk_src = TWAI_CLK_SRC_DEFAULT,
        .quanta_resolution_hz = 0,
        .brp = 1,
        .prop_seg = 5,
        .tseg_1 = 6,
        .tseg_2 = 4,
        .sjw = 3,
        .ssp_offset = 0,
        .triple_sampling = false,
    };
    error = twai_node_reconfig_timing(node_, &nominal, &data);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "exact 1M/5M timing configuration failed: %s", esp_err_to_name(error));
        (void)twai_node_delete(node_);
        node_ = nullptr;
        return false;
    }

    const twai_event_callbacks_t callbacks{
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
    };
    error = twai_node_register_event_callbacks(node_, &callbacks, this);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "callback registration failed: %s", esp_err_to_name(error));
        (void)twai_node_delete(node_);
        node_ = nullptr;
        return false;
    }
    return true;
}

bool EspTwaiTransport::start()
{
    if (node_ == nullptr || started_.load(std::memory_order_acquire)) {
        return false;
    }
    const esp_err_t error = twai_node_enable(node_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "enable failed: %s", esp_err_to_name(error));
        return false;
    }
    started_.store(true, std::memory_order_release);
    ESP_LOGI(kTag,
             "started: TX=GPIO%d RX=GPIO%d nominal=1M@80%%/SJW5 data=5M@75%%/SJW3",
             config_.tx_pin,
             config_.rx_pin);
    return true;
}

bool EspTwaiTransport::stop()
{
    if (node_ == nullptr) {
        return true;
    }
    started_.store(false, std::memory_order_release);
    const esp_err_t disable_result = twai_node_disable(node_);
    const esp_err_t delete_result = twai_node_delete(node_);
    node_ = nullptr;
    return (disable_result == ESP_OK || disable_result == ESP_ERR_INVALID_STATE) &&
           delete_result == ESP_OK;
}

can::SendResult EspTwaiTransport::send(const can::Frame& frame, uint32_t timeout_ms)
{
    if (!frame.valid()) {
        return can::SendResult::invalid_frame;
    }
    if (!started_.load(std::memory_order_acquire) || node_ == nullptr) {
        return can::SendResult::not_ready;
    }

    uint8_t slot_index_value = 0;
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(free_slots_queue_, &slot_index_value, ticks) != pdTRUE) {
        return timeout_ms == 0 ? can::SendResult::queue_full : can::SendResult::timeout;
    }

    TxSlot& slot = tx_slots_[slot_index_value];
    slot.native = {};
    slot.payload.fill(0);
    const std::size_t wire_size = frame.fd ? canonical_fd_length(frame.size) : frame.size;
    std::copy_n(frame.data.begin(), frame.size, slot.payload.begin());
    slot.native.header.id = frame.id;
    slot.native.header.ide = frame.extended;
    slot.native.header.rtr = frame.remote;
    slot.native.header.fdf = frame.fd;
    slot.native.header.brs = frame.bitrate_switch;
    slot.native.buffer = slot.payload.data();
    slot.native.buffer_len = wire_size;

    const esp_err_t result = twai_node_transmit(node_, &slot.native, timeout_ms);
    if (result != ESP_OK) {
        (void)xQueueSend(free_slots_queue_, &slot_index_value, 0);
        tx_failed_.fetch_add(1, std::memory_order_relaxed);
        if (result == ESP_ERR_TIMEOUT) {
            return can::SendResult::timeout;
        }
        if (result == ESP_ERR_INVALID_STATE) {
            return can::SendResult::not_ready;
        }
        return can::SendResult::io_error;
    }
    return can::SendResult::ok;
}

bool EspTwaiTransport::receive(can::Frame& frame, uint32_t timeout_ms)
{
    const TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return rx_queue_ != nullptr && xQueueReceive(rx_queue_, &frame, ticks) == pdTRUE;
}

void EspTwaiTransport::maintenance()
{
    if (!recovery_requested_.exchange(false, std::memory_order_acq_rel) || node_ == nullptr) {
        return;
    }
    const esp_err_t result = twai_node_recover(node_);
    if (result == ESP_OK) {
        recoveries_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(kTag, "bus-off recovery started");
    } else if (result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "bus-off recovery failed: %s", esp_err_to_name(result));
    }
}

TwaiStatistics EspTwaiTransport::statistics() const
{
    return {
        .rx_frames = rx_frames_.load(std::memory_order_relaxed),
        .rx_dropped = rx_dropped_.load(std::memory_order_relaxed),
        .tx_frames = tx_frames_.load(std::memory_order_relaxed),
        .tx_failed = tx_failed_.load(std::memory_order_relaxed),
        .bus_errors = bus_errors_.load(std::memory_order_relaxed),
        .recoveries = recoveries_.load(std::memory_order_relaxed),
    };
}

bool EspTwaiTransport::on_tx_done(twai_node_handle_t,
                                  const twai_tx_done_event_data_t* event,
                                  void* context)
{
    auto* self = static_cast<EspTwaiTransport*>(context);
    const int index = self->slot_index(event->done_tx_frame);
    if (index < 0) {
        self->tx_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (event->is_tx_success) {
        self->tx_frames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        self->tx_failed_.fetch_add(1, std::memory_order_relaxed);
    }
    const uint8_t slot = static_cast<uint8_t>(index);
    BaseType_t task_woken = pdFALSE;
    (void)xQueueSendFromISR(self->free_slots_queue_, &slot, &task_woken);
    return task_woken == pdTRUE;
}

bool EspTwaiTransport::on_rx_done(twai_node_handle_t node,
                                  const twai_rx_done_event_data_t*,
                                  void* context)
{
    auto* self = static_cast<EspTwaiTransport*>(context);
    std::array<uint8_t, can::kFdMaxPayload> payload{};
    twai_frame_t received{
        .header = {}, .buffer = payload.data(), .buffer_len = payload.size()};
    if (twai_node_receive_from_isr(node, &received) != ESP_OK) {
        self->rx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    can::Frame frame{};
    frame.id = received.header.id;
    frame.extended = received.header.ide;
    frame.remote = received.header.rtr;
    frame.fd = received.header.fdf;
    frame.bitrate_switch = received.header.brs;
    frame.size = static_cast<uint8_t>(twaifd_dlc2len(received.header.dlc));
    std::copy_n(payload.begin(), frame.size, frame.data.begin());

    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(self->rx_queue_, &frame, &task_woken) == pdTRUE) {
        self->rx_frames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        self->rx_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    return task_woken == pdTRUE;
}

bool EspTwaiTransport::on_state_change(twai_node_handle_t,
                                       const twai_state_change_event_data_t* event,
                                       void* context)
{
    auto* self = static_cast<EspTwaiTransport*>(context);
    if (event->new_sta == TWAI_ERROR_BUS_OFF) {
        self->recovery_requested_.store(true, std::memory_order_release);
    }
    return false;
}

bool EspTwaiTransport::on_error(twai_node_handle_t,
                                const twai_error_event_data_t*,
                                void* context)
{
    static_cast<EspTwaiTransport*>(context)->bus_errors_.fetch_add(1,
                                                                  std::memory_order_relaxed);
    return false;
}

std::size_t EspTwaiTransport::canonical_fd_length(std::size_t size)
{
    constexpr std::array<std::size_t, 8> lengths{8, 12, 16, 20, 24, 32, 48, 64};
    for (const std::size_t candidate : lengths) {
        if (size <= candidate) {
            return candidate;
        }
    }
    return 64;
}

int EspTwaiTransport::slot_index(const twai_frame_t* frame) const
{
    for (std::size_t index = 0; index < config_.tx_queue_depth; ++index) {
        if (&tx_slots_[index].native == frame) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace canopen_esp32
