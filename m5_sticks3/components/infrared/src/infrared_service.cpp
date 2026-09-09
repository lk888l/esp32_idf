#include "infrared_service.hpp"
#include "infrared_raw_mailbox.hpp"

#include <atomic>
#include <cstdio>
#include <new>
#include "bsp_board.hpp"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/soc_caps.h"

namespace infrared {
namespace {
constexpr char kTag[] = "infrared";
constexpr size_t kMaxSymbols = protocol::kMaxPulses / 2;
constexpr size_t kQueueDepth = 8;
constexpr uint32_t kReceiveIdleNs = 20000000;
constexpr int64_t kLearnTimeoutUs = 30000000;
constexpr int64_t kRepeatPeriodUs = 110000;
static_assert(SOC_RMT_SUPPORT_DMA, "StickS3 requires RMT DMA support");
enum class Operation : uint8_t { Listen, Learn, Nec, Raw, ReplayLast, ReplaySlot, Save, Erase, Carrier };
struct Command {
    Operation operation{};
    uint8_t slot = 0;
    uint16_t address = 0;
    uint8_t command = 0;
    uint8_t repeats = 0;
    bool extended = false;
    uint32_t carrier_hz = protocol::kDefaultCarrierHz;
    uint8_t duty_percent = 33;
};
struct ReceiveEvent { size_t count; };
void remember_error(esp_err_t value, esp_err_t& first) { if (first == ESP_OK && value != ESP_OK) first = value; }
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
}

struct Service::Impl {
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t finished = nullptr;
    QueueHandle_t commands = nullptr;
    QueueHandle_t rx_events = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> accepting{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> shutdown{false};
    std::atomic<uint32_t> lost_events{0};
    Snapshot status{};
    esp_err_t shutdown_result = ESP_OK;
    rmt_channel_handle_t tx = nullptr;
    rmt_channel_handle_t rx = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    rmt_symbol_word_t* rx_symbols = nullptr;
    rmt_symbol_word_t* tx_symbols = nullptr;
    bool hardware_ready = false;
    bool tx_enabled = false;
    bool rx_enabled = false;
    bool receive_requested = false;
    bool power_owned = false;
    bool receiver_owned = false;
    bool learning = false;
    uint8_t learn_slot = 0;
    int64_t learn_deadline = 0;
    protocol::Frame last{};
    protocol::Frame work{};
    RawFrameMailbox raw_mailbox;
    std::array<protocol::Frame, kSlotCount> slots{};
    std::array<uint8_t, protocol::kMaxSerializedBytes> storage{};
    protocol::NecDecoder decoder{};
    nvs_handle_t nvs = 0;

    esp_err_t submit(const Command& command) {
        Lock lock(mutex);
        if (!accepting.load() || cancel.load()) return ESP_ERR_INVALID_STATE;
        if (xQueueSend(commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        xTaskNotifyGive(task);
        return ESP_OK;
    }
    void set_state(State state, esp_err_t error = ESP_OK) {
        Lock lock(mutex);
        status.state = state;
        status.listening = state == State::Listening || state == State::Learning;
        status.learning = state == State::Learning;
        status.busy = status.listening || state == State::Transmitting;
        status.learning_slot = learn_slot;
        status.last_error = error;
        status.dropped_frames += lost_events.exchange(0);
    }
    static bool receive_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t* event, void* context) {
        auto* self = static_cast<Impl*>(context);
        const ReceiveEvent received{event->num_symbols};
        BaseType_t higher_priority_woken = pdFALSE;
        if (xQueueSendFromISR(self->rx_events, &received, &higher_priority_woken) != pdTRUE) ++self->lost_events;
        vTaskNotifyGiveFromISR(self->task, &higher_priority_woken);
        return higher_priority_woken == pdTRUE;
    }
    esp_err_t ensure_storage() {
        return nvs ? ESP_OK : nvs_open("infrared", NVS_READWRITE, &nvs);
    }
    esp_err_t ensure_hardware() {
        if (hardware_ready) return ESP_OK;
        const auto error = cleanup_hardware();
        if (error != ESP_OK) return error;
        const auto initialized = initialize_hardware();
        if (initialized != ESP_OK) {
            const auto cleanup = cleanup_hardware();
            if (cleanup != ESP_OK) ESP_LOGW(kTag, "RMT rollback pending: %s", esp_err_to_name(cleanup));
        }
        return initialized;
    }
    esp_err_t load_slots() {
        esp_err_t error = ensure_storage();
        if (error != ESP_OK) return error;
        for (size_t i = 0; i < kSlotCount; ++i) {
            char key[8]; std::snprintf(key, sizeof(key), "slot%u", unsigned(i));
            size_t size = storage.size();
            error = nvs_get_blob(nvs, key, storage.data(), &size);
            if (error == ESP_ERR_NVS_NOT_FOUND) continue;
            if (error != ESP_OK || !protocol::deserialize(storage.data(), size, slots[i])) {
                status.last_error = error == ESP_OK ? ESP_ERR_INVALID_CRC : error;
                ESP_LOGW(kTag, "Ignoring invalid stored slot %u", unsigned(i + 1));
                continue;
            }
            status.slot_used[i] = true;
        }
        return ESP_OK;
    }
    esp_err_t initialize_hardware() {
        rx_symbols = static_cast<rmt_symbol_word_t*>(heap_caps_calloc(kMaxSymbols, sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        tx_symbols = static_cast<rmt_symbol_word_t*>(heap_caps_calloc(kMaxSymbols, sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if (!rx_symbols || !tx_symbols) return ESP_ERR_NO_MEM;
        rmt_tx_channel_config_t tx_config{};
        tx_config.gpio_num = bsp::kIrTx;
        tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
        tx_config.resolution_hz = 1000000;
        tx_config.mem_block_symbols = kMaxSymbols;
        tx_config.trans_queue_depth = 1;
        tx_config.flags.with_dma = true;
        esp_err_t error = rmt_new_tx_channel(&tx_config, &tx);
        if (error != ESP_OK) return error;
        rmt_copy_encoder_config_t encoder_config{};
        error = rmt_new_copy_encoder(&encoder_config, &encoder);
        if (error != ESP_OK) return error;
        rmt_rx_channel_config_t rx_config{};
        rx_config.gpio_num = bsp::kIrRx;
        rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
        rx_config.resolution_hz = 1000000;
        rx_config.mem_block_symbols = kMaxSymbols;
        rx_config.flags.with_dma = true;
        error = rmt_new_rx_channel(&rx_config, &rx);
        if (error != ESP_OK) return error;
        rmt_rx_event_callbacks_t callbacks{};
        callbacks.on_recv_done = receive_done;
        error = rmt_rx_register_event_callbacks(rx, &callbacks, this);
        if (error == ESP_OK) { hardware_ready = true; Lock lock(mutex); status.dma_rx = true; status.dma_tx = true; }
        return error;
    }
    esp_err_t stop_io() {
        receive_requested = false;
        esp_err_t first = ESP_OK;
        if (rx_enabled) {
            const auto error = rmt_disable(rx);
            remember_error(error, first);
            if (error == ESP_OK) rx_enabled = false;
        }
        if (tx_enabled) {
            const auto error = rmt_disable(tx);
            remember_error(error, first);
            if (error == ESP_OK) tx_enabled = false;
        }
        // Keep ownership when hardware could not stop: do not let audio start
        // while an uncertain RMT reception still has a live buffer.
        if (!rx_enabled && receiver_owned) {
            const auto error = bsp::Board::instance().release_ir_receiver();
            remember_error(error, first);
            if (error == ESP_OK) receiver_owned = false;
        }
        if (!rx_enabled && !tx_enabled && power_owned) {
            const auto error = bsp::Board::instance().release_ir_power();
            remember_error(error, first);
            if (error == ESP_OK) power_owned = false;
        }
        if (!rx_enabled && rx_events) xQueueReset(rx_events);
        learning = false;
        decoder.reset();
        return first;
    }
    esp_err_t cleanup_hardware() {
        hardware_ready = false;
        if (mutex) { Lock lock(mutex); status.dma_rx = false; status.dma_tx = false; }
        esp_err_t first = stop_io();
        if (rx && !rx_enabled) {
            const auto error = rmt_del_channel(rx);
            remember_error(error, first);
            if (error == ESP_OK) rx = nullptr;
        }
        if (tx && !tx_enabled) {
            const auto error = rmt_del_channel(tx);
            remember_error(error, first);
            if (error == ESP_OK) tx = nullptr;
        }
        if (!tx && encoder) {
            const auto error = rmt_del_encoder(encoder);
            remember_error(error, first);
            if (error == ESP_OK) encoder = nullptr;
        }
        if (!rx && rx_symbols) { heap_caps_free(rx_symbols); rx_symbols = nullptr; }
        if (!tx && tx_symbols) { heap_caps_free(tx_symbols); tx_symbols = nullptr; }
        return first;
    }
    void free_software() {
        if (nvs) { nvs_close(nvs); nvs = 0; }
        if (commands) vQueueDelete(commands);
        if (rx_events) vQueueDelete(rx_events);
        if (finished) vSemaphoreDelete(finished);
        if (mutex) vSemaphoreDelete(mutex);
    }
    esp_err_t arm_receive() {
        rmt_receive_config_t config{};
        config.signal_range_min_ns = 1000;
        config.signal_range_max_ns = kReceiveIdleNs;
        return rmt_receive(rx, rx_symbols, kMaxSymbols * sizeof(*rx_symbols), &config);
    }
    esp_err_t start_receive(bool learn, uint8_t slot) {
        esp_err_t error = ensure_hardware();
        if (error != ESP_OK) return error;
        error = stop_io();
        if (error != ESP_OK) return error;
        error = bsp::Board::instance().acquire_ir_receiver();
        if (error != ESP_OK) return error;
        receiver_owned = true;
        error = bsp::Board::instance().acquire_ir_power();
        if (error != ESP_OK) { stop_io(); return error; }
        power_owned = true;
        error = rmt_enable(rx);
        if (error != ESP_OK) { stop_io(); return error; }
        rx_enabled = true;
        receive_requested = true;
        learning = learn;
        learn_slot = slot;
        learn_deadline = esp_timer_get_time() + kLearnTimeoutUs;
        error = arm_receive();
        if (error != ESP_OK) { stop_io(); return error; }
        set_state(learn ? State::Learning : State::Listening);
        return ESP_OK;
    }
    esp_err_t store_frame(uint8_t slot, const protocol::Frame& frame) {
        const size_t length = protocol::serialize(frame, storage.data(), storage.size());
        if (!length) return ESP_ERR_INVALID_ARG;
        char key[8]; std::snprintf(key, sizeof(key), "slot%u", unsigned(slot));
        esp_err_t error = ensure_storage();
        if (error == ESP_OK) error = nvs_set_blob(nvs, key, storage.data(), length);
        if (error == ESP_OK) error = nvs_commit(nvs);
        if (error == ESP_OK) {
            slots[slot] = frame;
            Lock lock(mutex); status.slot_used[slot] = true;
        }
        return error;
    }
    void receive_frame(const ReceiveEvent& event) {
        if (!rx_enabled || !receive_requested) return;
        bool valid = event.count > 0 && event.count < kMaxSymbols;
        work.count = 0;
        { Lock lock(mutex); work.carrier_hz = status.carrier_hz; work.duty_percent = status.duty_percent; }
        if (valid) {
            // Normalize either receiver polarity to marks-high. The first edge
            // captured by RMT is the beginning of an optical burst.
            const bool mark_level = rx_symbols[0].level0;
            for (size_t i = 0; i < event.count && valid; ++i) {
                const auto symbol = rx_symbols[i];
                const uint16_t durations[2] = {uint16_t(symbol.duration0), uint16_t(symbol.duration1)};
                const bool levels[2] = {bool(symbol.level0), bool(symbol.level1)};
                for (size_t half = 0; half < 2; ++half) {
                    if (!durations[half]) {
                        if (half == 0 && i + 1 < event.count) valid = false;
                        break;
                    }
                    if (work.count >= protocol::kMaxPulses) { valid = false; break; }
                    work.pulses[work.count++] = {durations[half], levels[half] == mark_level};
                }
            }
            valid = valid && protocol::valid_frame(work);
        }
        if (!valid) {
            Lock lock(mutex); ++status.dropped_frames;
        } else {
            const auto decoded = decoder.decode(work, uint64_t(esp_timer_get_time() / 1000));
            {
                Lock lock(mutex);
                ++status.received_frames;
                status.last_decoded = decoded;
                status.last_symbol_count = event.count;
                // A repeat alone cannot be replayed as a complete remote key.
                if (!decoded.repeat) last = work;
            }
            if (learning && !decoded.repeat) {
                const auto stop_error = stop_io();
                const auto error = stop_error == ESP_OK ? store_frame(learn_slot, work) : stop_error;
                set_state(error == ESP_OK ? State::Idle : State::Error, error);
                return;
            }
        }
        const auto error = arm_receive();
        if (error != ESP_OK) { stop_io(); set_state(State::Error, error); }
    }
    bool cancelled() const { return cancel.load() || shutdown.load(); }
    esp_err_t wait_until(int64_t deadline) {
        while (esp_timer_get_time() < deadline) {
            if (cancelled()) return ESP_ERR_INVALID_STATE;
            // A short notification wait bounds cancellation latency to one tick.
            ulTaskNotifyTake(pdTRUE, (pdMS_TO_TICKS(5) > 0 ? pdMS_TO_TICKS(5) : 1));
        }
        return cancelled() ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    esp_err_t transmit_frame(const protocol::Frame& frame) {
        if (cancelled()) return ESP_ERR_INVALID_STATE;
        if (!protocol::valid_frame(frame)) return ESP_ERR_INVALID_ARG;
        const size_t symbol_count = (frame.count + 1) / 2;
        for (size_t i = 0; i < symbol_count; ++i) {
            tx_symbols[i] = {};
            const auto& first = frame.pulses[2 * i];
            tx_symbols[i].duration0 = first.duration_us;
            tx_symbols[i].level0 = first.mark;
            if (2 * i + 1 < frame.count) {
                const auto& second = frame.pulses[2 * i + 1];
                tx_symbols[i].duration1 = second.duration_us;
                tx_symbols[i].level1 = second.mark;
            }
        }
        rmt_carrier_config_t carrier{};
        carrier.frequency_hz = frame.carrier_hz;
        carrier.duty_cycle = frame.duty_percent / 100.0F;
        esp_err_t error = rmt_apply_carrier(tx, &carrier);
        if (error != ESP_OK) return error;
        rmt_transmit_config_t config{};
        config.flags.eot_level = 0;
        error = rmt_transmit(tx, encoder, tx_symbols, symbol_count * sizeof(*tx_symbols), &config);
        if (error != ESP_OK) return error;
        const int64_t deadline = esp_timer_get_time() + protocol::duration_us(frame) + 200000;
        do {
            if (cancelled()) return ESP_ERR_INVALID_STATE;
            error = rmt_tx_wait_all_done(tx, 10);
            if (error == ESP_OK) {
                Lock lock(mutex); ++status.transmitted_frames;
                return ESP_OK;
            }
            if (error != ESP_ERR_TIMEOUT) return error;
        } while (esp_timer_get_time() < deadline);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t transmit(uint8_t repeats) {
        esp_err_t error = ensure_hardware();
        if (error != ESP_OK) return error;
        error = stop_io();
        if (error != ESP_OK) return error;
        error = bsp::Board::instance().acquire_ir_power();
        if (error != ESP_OK) return error;
        power_owned = true;
        error = rmt_enable(tx);
        if (error != ESP_OK) { stop_io(); return error; }
        tx_enabled = true;
        set_state(State::Transmitting);
        int64_t start = esp_timer_get_time();
        error = transmit_frame(work);
        if (error == ESP_OK && repeats) {
            const auto carrier_hz = work.carrier_hz;
            const auto duty_percent = work.duty_percent;
            protocol::encode_nec_repeat(work);
            work.carrier_hz = carrier_hz; work.duty_percent = duty_percent;
            for (unsigned i = 0; i < repeats && error == ESP_OK; ++i) {
                start += kRepeatPeriodUs;
                error = wait_until(start);
                if (error == ESP_OK) error = transmit_frame(work);
            }
        }
        remember_error(stop_io(), error);
        return error;
    }
    void execute(const Command& command) {
        if (cancelled()) return;
        esp_err_t error = ESP_OK;
        switch (command.operation) {
        case Operation::Listen: error = start_receive(false, 0); break;
        case Operation::Learn: error = start_receive(true, command.slot); break;
        case Operation::Nec:
            if (!protocol::encode_nec(command.address, command.command, command.extended, work)) error = ESP_ERR_INVALID_ARG;
            else {
                { Lock lock(mutex); work.carrier_hz = status.carrier_hz; work.duty_percent = status.duty_percent; }
                error = transmit(command.repeats);
            }
            if (error == ESP_OK) set_state(State::Idle);
            break;
        case Operation::Raw: {
            bool present = false;
            { Lock lock(mutex); present = raw_mailbox.try_take(work); }
            error = present ? transmit(0) : ESP_ERR_INVALID_STATE;
            if (error == ESP_OK) set_state(State::Idle);
            break;
        }
        case Operation::ReplayLast:
            { Lock lock(mutex); work = last; }
            error = protocol::valid_frame(work) ? transmit(0) : ESP_ERR_NOT_FOUND;
            if (error == ESP_OK) set_state(State::Idle);
            break;
        case Operation::ReplaySlot:
            work = slots[command.slot];
            error = protocol::valid_frame(work) ? transmit(0) : ESP_ERR_NOT_FOUND;
            if (error == ESP_OK) set_state(State::Idle);
            break;
        case Operation::Save:
            { Lock lock(mutex); work = last; }
            error = work.count ? store_frame(command.slot, work) : ESP_ERR_NOT_FOUND;
            break;
        case Operation::Erase: {
            char key[8]; std::snprintf(key, sizeof(key), "slot%u", unsigned(command.slot));
            error = ensure_storage();
            if (error == ESP_OK) error = nvs_erase_key(nvs, key);
            if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
            if (error == ESP_OK) error = nvs_commit(nvs);
            if (error == ESP_OK) {
                slots[command.slot].count = 0;
                Lock lock(mutex); status.slot_used[command.slot] = false;
            }
            break;
        }
        case Operation::Carrier: {
            Lock lock(mutex);
            status.carrier_hz = command.carrier_hz;
            status.duty_percent = command.duty_percent;
            break;
        }
        }
        if (error != ESP_OK && !cancelled()) {
            ESP_LOGW(kTag, "Command %u failed: %s", unsigned(command.operation), esp_err_to_name(error));
            stop_io(); set_state(State::Error, error);
        } else if (error == ESP_OK) {
            const bool admin = command.operation == Operation::Save || command.operation == Operation::Erase || command.operation == Operation::Carrier;
            if (admin) {
                const auto cleanup = (!rx_enabled && !tx_enabled) ? stop_io() : ESP_OK;
                set_state(cleanup != ESP_OK ? State::Error : rx_enabled ? (learning ? State::Learning : State::Listening) : State::Idle, cleanup);
            } else { Lock lock(mutex); status.last_error = ESP_OK; }
        }
    }
    static void task_entry(void* context) {
        auto* self = static_cast<Impl*>(context);
        self->run();
        const auto done = self->finished;
        xSemaphoreGive(done);
        vTaskSuspend(nullptr);
    }
    void run() {
        while (!shutdown.load()) {
            if (cancel.load()) {
                { Lock lock(mutex); xQueueReset(commands); raw_mailbox.clear(); cancel.store(false); }
                const auto error = stop_io();
                set_state(error == ESP_OK ? State::Idle : State::Error, error);
            }
            ReceiveEvent event{};
            if (xQueueReceive(rx_events, &event, 0) == pdTRUE) receive_frame(event);
            if (learning && esp_timer_get_time() >= learn_deadline) {
                stop_io(); set_state(State::Error, ESP_ERR_TIMEOUT);
            }
            Command command{};
            if (!cancel.load() && xQueueReceive(commands, &command, 0) == pdTRUE) execute(command);
            else ulTaskNotifyTake(pdTRUE, (pdMS_TO_TICKS(20) > 0 ? pdMS_TO_TICKS(20) : 1));
        }
        shutdown_result = cleanup_hardware();
        set_state(shutdown_result == ESP_OK ? State::Idle : State::Error, shutdown_result);
    }
};

Service& Service::instance() { static Service service; return service; }
esp_err_t Service::initialize() {
    if (impl_) return impl_->accepting.load() ? ESP_OK : ESP_ERR_INVALID_STATE;
    auto* state = new (std::nothrow) Impl;
    if (!state) return ESP_ERR_NO_MEM;
    state->mutex = xSemaphoreCreateMutex();
    state->finished = xSemaphoreCreateBinary();
    state->commands = xQueueCreate(kQueueDepth, sizeof(Command));
    state->rx_events = xQueueCreate(1, sizeof(ReceiveEvent));
    esp_err_t error = state->mutex && state->finished && state->commands && state->rx_events ? ESP_OK : ESP_ERR_NO_MEM;
    if (error == ESP_OK) {
        const auto storage_error = state->load_slots();
        if (storage_error != ESP_OK) {
            state->status.last_error = storage_error;
            state->status.state = State::Error;
            ESP_LOGW(kTag, "Storage unavailable; capture/transmit remain usable: %s", esp_err_to_name(storage_error));
        }
    }
    if (error == ESP_OK) {
        state->status.initialized = true;
        if (xTaskCreate(Impl::task_entry, "infrared", 6144, state, 5, &state->task) != pdPASS) error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        const auto cleanup = state->cleanup_hardware();
        if (cleanup != ESP_OK) { impl_ = state; return cleanup; }
        state->free_software(); delete state;
        return error;
    }
    impl_ = state;
    state->accepting.store(true);
    return ESP_OK;
}
esp_err_t Service::deinitialize() {
    if (!impl_) return ESP_OK;
    auto* state = impl_;
    state->accepting.store(false);
    if (state->task) {
        state->shutdown.store(true); state->cancel.store(true);
        xTaskNotifyGive(state->task);
        if (xSemaphoreTake(state->finished, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        vTaskDelete(state->task); state->task = nullptr;
    }
    const auto error = state->cleanup_hardware();
    if (error != ESP_OK) return error;
    state->free_software(); delete state; impl_ = nullptr;
    return ESP_OK;
}
esp_err_t Service::listen() { return impl_ ? impl_->submit({Operation::Listen}) : ESP_ERR_INVALID_STATE; }
esp_err_t Service::stop() {
    if (!impl_ || !impl_->accepting.load()) return ESP_ERR_INVALID_STATE;
    Lock lock(impl_->mutex);
    impl_->cancel.store(true); xTaskNotifyGive(impl_->task); return ESP_OK;
}
esp_err_t Service::learn(uint8_t slot) {
    if (slot >= kSlotCount) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::Learn; command.slot = slot;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
esp_err_t Service::transmit_nec(uint16_t address, uint8_t command_byte, bool extended, uint8_t repeats) {
    if ((!extended && address > 0xFF) || repeats > 20) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::Nec; command.address = address;
    command.command = command_byte; command.extended = extended; command.repeats = repeats;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
esp_err_t Service::transmit_raw(const protocol::Frame& frame) {
    if (!protocol::valid_frame(frame)) return ESP_ERR_INVALID_ARG;
    if (!impl_) return ESP_ERR_INVALID_STATE;
    Lock lock(impl_->mutex);
    if (!impl_->accepting.load() || impl_->cancel.load()) return ESP_ERR_INVALID_STATE;
    if (!impl_->raw_mailbox.try_put(frame)) return ESP_ERR_INVALID_STATE;
    const Command command{Operation::Raw};
    if (xQueueSend(impl_->commands, &command, 0) != pdTRUE) {
        impl_->raw_mailbox.clear();
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotifyGive(impl_->task);
    return ESP_OK;
}
esp_err_t Service::replay_last() { return impl_ ? impl_->submit({Operation::ReplayLast}) : ESP_ERR_INVALID_STATE; }
esp_err_t Service::replay_slot(uint8_t slot) {
    if (slot >= kSlotCount) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::ReplaySlot; command.slot = slot;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
esp_err_t Service::save_last(uint8_t slot) {
    if (slot >= kSlotCount) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::Save; command.slot = slot;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
esp_err_t Service::erase_slot(uint8_t slot) {
    if (slot >= kSlotCount) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::Erase; command.slot = slot;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
esp_err_t Service::set_carrier(uint32_t frequency_hz, uint8_t duty_percent) {
    if (frequency_hz < 20000 || frequency_hz > 60000 || !duty_percent || duty_percent > 50) return ESP_ERR_INVALID_ARG;
    Command command{}; command.operation = Operation::Carrier; command.carrier_hz = frequency_hz; command.duty_percent = duty_percent;
    return impl_ ? impl_->submit(command) : ESP_ERR_INVALID_STATE;
}
Snapshot Service::snapshot() const {
    if (!impl_ || !impl_->mutex) return {};
    Lock lock(impl_->mutex); return impl_->status;
}
esp_err_t Service::copy_last_frame(protocol::Frame& output) const {
    if (!impl_ || !impl_->accepting.load()) return ESP_ERR_INVALID_STATE;
    Lock lock(impl_->mutex);
    if (!impl_->last.count) return ESP_ERR_NOT_FOUND;
    output = impl_->last;
    return ESP_OK;
}
} // namespace infrared
