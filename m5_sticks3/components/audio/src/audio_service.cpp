#include "audio_service.hpp"
#include "audio_dsp.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "bsp_board.hpp"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace audio {
namespace {

constexpr char kTag[] = "audio";
constexpr size_t kChunkFrames = 192;
constexpr uint32_t kDmaDescriptors = 6;
constexpr uint32_t kIoTimeoutMs = 50;
constexpr uint32_t kMaxRecordingMs = 30000;
constexpr size_t kQueueDepth = 8;

enum class Operation : uint8_t { Tone, Meter, Record, Playback, Clear, Stream, Volume, Gain, Rate, Bits, Format };
struct Command {
    Operation operation;
    uint32_t epoch = 0;
    uint32_t value = 0;
    uint32_t duration_ms = 0;
    float gain = 0;
    StreamCallbacks stream = {};
};

// All hardware and buffer ownership stays on the worker. The short spinlock
// protects only queue lifetime and small value snapshots, never hardware I/O.
struct Context {
    portMUX_TYPE gate = portMUX_INITIALIZER_UNLOCKED;
    QueueHandle_t commands = nullptr;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t worker = nullptr;
    bool accepting = false;
    bool running = false; // Protected by gate; prevents notifying a deleted worker.
    std::atomic<bool> quit{false};
    std::atomic<uint32_t> epoch{0};
    std::atomic<uint32_t> overruns{0};
    Snapshot published;
    Snapshot status;
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    const audio_codec_ctrl_if_t* control = nullptr;
    const audio_codec_data_if_t* data = nullptr;
    const audio_codec_gpio_if_t* gpio = nullptr;
    const audio_codec_if_t* codec = nullptr;
    esp_codec_dev_handle_t device = nullptr;
    bool opened = false;
    bool speaker = false;
    uint8_t* recording = nullptr;
    size_t recording_bytes = 0;
    size_t recording_capacity = 0;
    uint64_t position = 0;
    uint64_t total_frames = 0;
    uint32_t tone_hz = 1000;
    size_t discard_frames = 0;
    int64_t drain_until_us = 0;
    int64_t power_check_us = 0;
    StreamCallbacks stream;
    alignas(4) uint8_t output[kChunkFrames * sizeof(int32_t)] = {};
    alignas(4) uint8_t input[kChunkFrames * sizeof(int32_t)] = {};
};
Context context;

void publish()
{
    context.status.rx_overruns = context.overruns.load(std::memory_order_relaxed);
    portENTER_CRITICAL(&context.gate);
    context.status.stop_generation = context.epoch.load(std::memory_order_relaxed);
    context.published = context.status;
    portEXIT_CRITICAL(&context.gate);
}

void retain_first_error(esp_err_t& result, esp_err_t next)
{
    if (result == ESP_OK && next != ESP_OK) result = next;
}

bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    context.overruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

esp_err_t disable_and_delete(i2s_chan_handle_t& channel)
{
    if (!channel) return ESP_OK;
    i2s_chan_info_t information = {};
    esp_err_t result = i2s_channel_get_info(channel, &information);
    if (result == ESP_OK && information.is_enabled) result = i2s_channel_disable(channel);
    if (result == ESP_OK) {
        result = i2s_del_channel(channel);
        if (result == ESP_OK) channel = nullptr;
    }
    return result;
}

esp_err_t close_hardware()
{
    if (!context.tx && !context.rx && !context.control && !context.data &&
        !context.gpio && !context.codec && !context.device && !context.speaker) return ESP_OK;
    // Disable the physical amplifier first, even after a failed codec open.
    esp_err_t result = bsp::Board::instance().set_speaker_enabled(false);
    if (result == ESP_OK) context.speaker = false;
    if (context.device) {
        if (context.opened) retain_first_error(result, esp_codec_dev_set_out_mute(context.device, true));
        retain_first_error(result, esp_codec_dev_close(context.device));
        esp_codec_dev_delete(context.device);
        context.device = nullptr;
        context.opened = false;
    }
    if (context.codec) {
        retain_first_error(result, audio_codec_delete_codec_if(context.codec));
        context.codec = nullptr;
    }
    if (context.data) {
        retain_first_error(result, audio_codec_delete_data_if(context.data));
        context.data = nullptr;
    }
    if (context.control) {
        retain_first_error(result, audio_codec_delete_ctrl_if(context.control));
        context.control = nullptr;
    }
    if (context.gpio) {
        retain_first_error(result, audio_codec_delete_gpio_if(context.gpio));
        context.gpio = nullptr;
    }
    retain_first_error(result, disable_and_delete(context.rx));
    retain_first_error(result, disable_and_delete(context.tx));
    context.drain_until_us = 0;
    context.stream = {};
    return result;
}

void finish(esp_err_t error = ESP_OK)
{
    retain_first_error(error, close_hardware());
    context.status.state = error == ESP_OK ? State::Idle : State::Fault;
    context.status.last_error = error;
    context.status.rms_dbfs = -96.0F;
    context.status.peak_dbfs = -96.0F;
    publish();
    if (error != ESP_OK) ESP_LOGW(kTag, "audio stopped: %s", esp_err_to_name(error));
}

esp_err_t refresh_volume()
{
    bsp::PowerStatus power;
    const bool externally_powered = bsp::Board::instance().read_power_status(power) == ESP_OK && power.externally_powered();
    const uint8_t effective = std::min<uint8_t>(context.status.volume_percent, externally_powered ? 100 : 70);
    const esp_err_t result = context.device ? esp_codec_dev_set_out_vol(context.device, effective) : ESP_OK;
    if (result == ESP_OK) context.status.effective_volume_percent = effective;
    context.power_check_us = esp_timer_get_time();
    return result;
}

esp_err_t open_hardware(bool speaker)
{
    if (!bsp::Board::instance().initialized()) return ESP_ERR_INVALID_STATE;
    // One paired allocation is essential: independently allocated channels may
    // use different peripherals and cannot share the ES8311 BCLK/LRCK pins.
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = kDmaDescriptors;
    channel.dma_frame_num = kChunkFrames;
    channel.auto_clear = true;
    esp_err_t result = i2s_new_channel(&channel, &context.tx, &context.rx);
    if (result != ESP_OK) return result;

    // A 32-bit I2S word transports all 24 ES8311 ADC/DAC bits. Keeping 32-bit
    // slots allows MCLK=256*Fs at every rate, including 24/44.1 kHz for which
    // the ES8311 coefficient table does not offer MCLK=384*Fs.
    const auto wire_bits = context.status.bits_per_sample == 16 ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT;
    i2s_std_config_t standard = {};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(context.status.sample_rate_hz);
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(wire_bits, I2S_SLOT_MODE_MONO);
    standard.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    standard.gpio_cfg.mclk = bsp::kAudioMclk;
    standard.gpio_cfg.bclk = bsp::kAudioBclk;
    standard.gpio_cfg.ws = bsp::kAudioWs;
    standard.gpio_cfg.dout = bsp::kAudioDout;
    standard.gpio_cfg.din = bsp::kAudioDin;
    result = i2s_channel_init_std_mode(context.tx, &standard);
    if (result != ESP_OK) return result;
    result = i2s_channel_init_std_mode(context.rx, &standard);
    if (result != ESP_OK) return result;
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = on_rx_overflow;
    result = i2s_channel_register_event_callback(context.rx, &callbacks, nullptr);
    if (result != ESP_OK) return result;

    audio_codec_i2c_cfg_t i2c = {};
    i2c.port = bsp::kInternalI2cPort;
    // esp_codec_dev uses an 8-bit wire address, whereas BSP uses 7-bit I2C.
    i2c.addr = bsp::kEs8311Address << 1;
    i2c.bus_handle = bsp::Board::instance().i2c_bus();
    i2c.clock_speed_hz = bsp::kInternalI2cClockHz;
    context.control = audio_codec_new_i2c_ctrl(&i2c);
    if (!context.control) return ESP_ERR_NO_MEM;
    audio_codec_i2s_cfg_t i2s = {};
    i2s.port = I2S_NUM_0;
    i2s.tx_handle = context.tx;
    i2s.rx_handle = context.rx;
    context.data = audio_codec_new_i2s_data(&i2s);
    context.gpio = audio_codec_new_gpio();
    if (!context.data || !context.gpio) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t codec = {};
    codec.ctrl_if = context.control;
    codec.gpio_if = context.gpio;
    codec.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codec.pa_pin = -1; // AW8737 enable belongs to M5PM1, not an ESP GPIO.
    codec.use_mclk = true;
    codec.no_dac_ref = true;
    codec.mclk_div = 256;
    context.codec = es8311_codec_new(&codec);
    if (!context.codec) return ESP_FAIL;
    esp_codec_dev_cfg_t device = {};
    device.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    device.codec_if = context.codec;
    device.data_if = context.data;
    context.device = esp_codec_dev_new(&device);
    if (!context.device) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = wire_bits;
    format.channel = 1;
    format.channel_mask = 1;
    format.sample_rate = context.status.sample_rate_hz;
    format.mclk_multiple = 256;
    result = esp_codec_dev_open(context.device, &format);
    if (result != ESP_OK) return result;
    context.opened = true;
    result = esp_codec_dev_set_out_mute(context.device, true);
    if (result == ESP_OK) result = esp_codec_dev_set_in_gain(context.device, context.status.microphone_gain_db);
    if (result == ESP_OK) result = refresh_volume();
    if (result != ESP_OK) return result;
    // Zero preload prevents a stale DMA buffer from reaching the amplifier.
    std::memset(context.output, 0, sizeof(context.output));
    size_t written = 0;
    result = i2s_channel_write(context.tx, context.output,
        kChunkFrames * dsp::sample_bytes(context.status.bits_per_sample), &written, kIoTimeoutMs);
    if (result != ESP_OK) return result;
    if (speaker) {
        context.speaker = true; // Keep ownership until physical shutdown succeeds.
        result = bsp::Board::instance().set_speaker_enabled(true);
        if (result != ESP_OK) return result; // IR receive owns the PA-off lease.
        context.speaker = true;
        result = esp_codec_dev_set_out_mute(context.device, false);
    }
    context.discard_frames = context.status.sample_rate_hz / 20; // 50 ms ADC settling.
    return result;
}

void clear_clip()
{
    heap_caps_free(context.recording);
    context.recording = nullptr;
    context.recording_bytes = 0;
    context.recording_capacity = 0;
    context.status.recording_available = false;
    context.status.recorded_ms = 0;
}

esp_err_t enqueue(Command command)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&context.gate);
    if (context.accepting && context.commands) {
        command.epoch = context.epoch.load(std::memory_order_relaxed);
        result = xQueueSend(context.commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
        if (result == ESP_OK && context.worker) xTaskNotifyGive(context.worker);
    }
    portEXIT_CRITICAL(&context.gate);
    return result;
}

void execute(const Command& command)
{
    // Settings which are safe during an activity do not interrupt it.
    if (command.operation == Operation::Volume || command.operation == Operation::Gain) {
        esp_err_t result = ESP_OK;
        if (command.operation == Operation::Volume) {
            context.status.volume_percent = command.value;
            result = refresh_volume();
        } else {
            context.status.microphone_gain_db = dsp::quantize_gain(command.gain);
            if (context.device) result = esp_codec_dev_set_in_gain(context.device, context.status.microphone_gain_db);
        }
        if (result != ESP_OK) finish(result); else publish();
        return;
    }

    const esp_err_t cleanup = close_hardware();
    if (cleanup != ESP_OK) { finish(cleanup); return; }
    context.status.state = State::Idle;
    context.status.last_error = ESP_OK;
    context.status.position_ms = 0;
    context.status.duration_ms = 0;
    context.status.rms_dbfs = context.status.peak_dbfs = -96.0F;
    context.position = 0;
    context.total_frames = 0;
    if (command.operation == Operation::Clear || command.operation == Operation::Rate || command.operation == Operation::Bits || command.operation == Operation::Format) {
        clear_clip();
        if (command.operation == Operation::Rate) context.status.sample_rate_hz = command.value;
        if (command.operation == Operation::Bits) context.status.bits_per_sample = command.value;
        if (command.operation == Operation::Format) {
            context.status.sample_rate_hz = command.value;
            context.status.bits_per_sample = command.duration_ms;
        }
        publish();
        return;
    }
    if (command.operation == Operation::Playback && !context.recording_bytes) { finish(ESP_ERR_NOT_FOUND); return; }
    if (command.operation == Operation::Tone && command.value >= context.status.sample_rate_hz / 2) { finish(ESP_ERR_INVALID_ARG); return; }
    if (command.operation == Operation::Record) {
        const size_t bytes = static_cast<uint64_t>(context.status.sample_rate_hz) * command.duration_ms / 1000 * dsp::sample_bytes(context.status.bits_per_sample);
        // Allocate the replacement before discarding a useful previous recording.
        auto* replacement = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!replacement) { finish(ESP_ERR_NO_MEM); return; }
        clear_clip();
        context.recording = replacement;
        context.recording_capacity = bytes;
    }
    const bool speaker = command.operation == Operation::Tone || command.operation == Operation::Playback ||
        (command.operation == Operation::Stream && command.stream.source);
    const esp_err_t result = open_hardware(speaker);
    if (result != ESP_OK) { finish(result); return; }
    context.overruns.store(0, std::memory_order_relaxed);
    context.status.clipped_samples = 0;
    context.status.tx_underruns = 0;
    switch (command.operation) {
    case Operation::Tone:
        context.status.state = State::Tone;
        context.tone_hz = command.value;
        context.total_frames = static_cast<uint64_t>(context.status.sample_rate_hz) * command.duration_ms / 1000;
        break;
    case Operation::Meter: context.status.state = State::Meter; break;
    case Operation::Record:
        context.status.state = State::Recording;
        context.total_frames = context.recording_capacity / dsp::sample_bytes(context.status.bits_per_sample);
        break;
    case Operation::Playback:
        context.status.state = State::Playback;
        context.total_frames = context.recording_bytes / dsp::sample_bytes(context.status.bits_per_sample);
        break;
    case Operation::Stream:
        context.status.state = State::Streaming;
        context.stream = command.stream;
        break;
    default: break;
    }
    context.status.duration_ms = context.total_frames * 1000 / context.status.sample_rate_hz;
    publish();
}

void begin_drain()
{
    // write() completes when queued to DMA, not when audible. Keep clocks/PA
    // alive for the worst-case ring residence; auto_clear transmits silence.
    context.drain_until_us = esp_timer_get_time() +
        static_cast<int64_t>((kDmaDescriptors + 1) * kChunkFrames) * 1000000 / context.status.sample_rate_hz;
}

void process_chunk()
{
    if (context.drain_until_us) {
        if (esp_timer_get_time() >= context.drain_until_us) { finish(); return; }
        // Drain RX as well to avoid artificial receive-overrun diagnostics.
        size_t received = 0;
        const esp_err_t result = i2s_channel_read(context.rx, context.input,
            kChunkFrames * dsp::sample_bytes(context.status.bits_per_sample), &received, kIoTimeoutMs);
        if (result != ESP_OK) finish(result);
        return;
    }
    const auto state = context.status.state;
    const size_t sample_size = dsp::sample_bytes(context.status.bits_per_sample);
    size_t frames = kChunkFrames;
    if (context.total_frames) frames = std::min<uint64_t>(frames, context.total_frames - context.position);
    if (!frames) { if (context.speaker) begin_drain(); else finish(); return; }
    std::memset(context.output, 0, frames * sample_size);
    if (state == State::Tone) {
        dsp::tone(context.output, frames, context.status.bits_per_sample, context.status.sample_rate_hz,
            context.tone_hz, context.position, context.total_frames);
    } else if (state == State::Playback) {
        std::memcpy(context.output, context.recording + context.position * sample_size, frames * sample_size);
    } else if (state == State::Streaming && context.stream.source) {
        frames = context.stream.source(context.stream.context, context.output, frames);
        if (!frames) { begin_drain(); return; }
        if (frames > kChunkFrames) { finish(ESP_ERR_INVALID_SIZE); return; }
    }
    size_t written = 0;
    esp_err_t result = i2s_channel_write(context.tx, context.output, frames * sample_size, &written, kIoTimeoutMs);
    if (result != ESP_OK || written != frames * sample_size) {
        ++context.status.tx_underruns;
        finish(result == ESP_OK ? ESP_ERR_INVALID_SIZE : result);
        return;
    }
    size_t received = 0;
    // Direct I2S calls supply a bounded timeout (the codec convenience read /
    // write helpers use their own long timeout). Both directions always run.
    result = i2s_channel_read(context.rx, context.input, frames * sample_size, &received, kIoTimeoutMs);
    if (result != ESP_OK || received != frames * sample_size) {
        finish(result == ESP_OK ? ESP_ERR_INVALID_SIZE : result);
        return;
    }
    if (context.discard_frames && (state == State::Meter || state == State::Recording ||
        (state == State::Streaming && !context.stream.source))) {
        const size_t skip = std::min(context.discard_frames, frames);
        context.discard_frames -= skip;
        frames -= skip;
        std::memmove(context.input, context.input + skip * sample_size, frames * sample_size);
    }
    if (!frames) return;
    // In 32-bit slots ES8311 has 24 significant bits; normalize padding before
    // exposing PCM so files and callbacks get a stable signed 24-bit format.
    if (context.status.bits_per_sample == 24) {
        for (size_t i = 0; i < frames; ++i) context.input[i * 4] = 0;
    }
    if (state == State::Meter || state == State::Recording || (state == State::Streaming && context.stream.sink)) {
        const auto levels = dsp::measure(context.input, frames, context.status.bits_per_sample);
        context.status.rms_dbfs = levels.rms_dbfs;
        context.status.peak_dbfs = levels.peak_dbfs;
        context.status.clipped_samples += levels.clipped;
    }
    if (state == State::Recording) {
        std::memcpy(context.recording + context.recording_bytes, context.input, frames * sample_size);
        context.recording_bytes += frames * sample_size;
        context.status.recording_available = true;
        context.status.recorded_ms = context.recording_bytes / sample_size * 1000 / context.status.sample_rate_hz;
    }
    if (state == State::Streaming && context.stream.sink) {
        result = context.stream.sink(context.stream.context, context.input, frames);
        if (result != ESP_OK) { finish(result); return; }
    }
    context.position += frames;
    context.status.position_ms = std::min<uint64_t>(UINT32_MAX, context.position * 1000 / context.status.sample_rate_hz);
    if (context.speaker && esp_timer_get_time() - context.power_check_us >= 1000000) {
        result = refresh_volume();
        if (result != ESP_OK) { finish(result); return; }
    }
    publish();
    if (context.total_frames && context.position >= context.total_frames) {
        if (context.speaker) begin_drain(); else finish();
    }
}

void worker(void*)
{
    uint32_t observed_epoch = context.status.settled_generation;
    while (!context.quit.load(std::memory_order_acquire)) {
        const uint32_t epoch = context.epoch.load(std::memory_order_acquire);
        if (epoch != observed_epoch) {
            finish();
            observed_epoch = epoch;
            context.status.settled_generation = epoch;
            publish();
        }
        Command command;
        // Process at most one command per chunk to prevent control traffic
        // starving an active DMA stream. Stale commands cannot restart audio.
        // Serialize dequeue with stop/submit: a command accepted AFTER a new
        // stop must wait until that generation is acknowledged, not be opened
        // under the previous generation and immediately cancelled again.
        portENTER_CRITICAL(&context.gate);
        const bool have_command = observed_epoch == context.epoch.load(std::memory_order_relaxed) &&
            xQueueReceive(context.commands, &command, 0) == pdTRUE;
        portEXIT_CRITICAL(&context.gate);
        if (have_command) {
            if (command.epoch == context.epoch.load(std::memory_order_acquire)) execute(command);

        }
        if (context.quit.load() || observed_epoch != context.epoch.load()) continue;
        if (context.opened) process_chunk();
        else ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    finish();
    clear_clip();
    context.status.settled_generation = context.epoch.load();
    context.status.initialized = false;
    context.status.state = State::Unavailable;
    publish();
    portENTER_CRITICAL(&context.gate);
    context.running = false;
    portEXIT_CRITICAL(&context.gate);
    xSemaphoreGive(context.done);
    vTaskDelete(nullptr);
}

} // namespace

Service& Service::instance()
{
    static Service service;
    return service;
}

esp_err_t Service::initialize()
{
    // Lifecycle serialization is an application-owner contract, as for BSP.
    if (context.worker) return context.accepting ? ESP_OK : ESP_ERR_INVALID_STATE;
    const esp_err_t cleanup = close_hardware();
    if (cleanup != ESP_OK) return cleanup;
    context.commands = xQueueCreate(kQueueDepth, sizeof(Command));
    context.done = xSemaphoreCreateBinary();
    if (!context.commands || !context.done) {
        if (context.commands) vQueueDelete(context.commands);
        if (context.done) vSemaphoreDelete(context.done);
        context.commands = nullptr;
        context.done = nullptr;
        return ESP_ERR_NO_MEM;
    }
    context.quit.store(false);
    context.epoch.store(0);
    context.status = {};
    context.status.initialized = true;
    context.status.state = State::Idle;
    publish();
    context.running = true;
    if (xTaskCreate(worker, "audio_service", 6144, nullptr, 5, &context.worker) != pdPASS) {
        vQueueDelete(context.commands);
        vSemaphoreDelete(context.done);
        context.commands = nullptr;
        context.done = nullptr;
        context.worker = nullptr;
        context.running = false;
        context.status = {};
        publish();
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&context.gate);
    context.accepting = true;
    portEXIT_CRITICAL(&context.gate);
    ESP_LOGI(kTag, "audio worker ready: paired I2S DMA, ES8311, PSRAM recording");
    return ESP_OK;
}

esp_err_t Service::deinitialize()
{
    if (!context.worker) return close_hardware();
    if (xTaskGetCurrentTaskHandle() == context.worker) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&context.gate);
    context.accepting = false;
    context.quit.store(true, std::memory_order_release);
    if (context.running) xTaskNotifyGive(context.worker);
    portEXIT_CRITICAL(&context.gate);
    if (xSemaphoreTake(context.done, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    context.worker = nullptr;
    vQueueDelete(context.commands);
    vSemaphoreDelete(context.done);
    context.commands = nullptr;
    context.done = nullptr;
    return context.status.last_error;
}

esp_err_t Service::play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (frequency_hz < 20 || frequency_hz > 4000 || duration_ms < 100 || duration_ms > 30000) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Tone, .value = frequency_hz, .duration_ms = duration_ms});
}
esp_err_t Service::start_meter() { return enqueue({.operation = Operation::Meter}); }
esp_err_t Service::start_recording(uint32_t duration_ms)
{
    if (duration_ms < 100 || duration_ms > kMaxRecordingMs) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Record, .duration_ms = duration_ms});
}
esp_err_t Service::play_recording() { return enqueue({.operation = Operation::Playback}); }
esp_err_t Service::clear_recording() { return enqueue({.operation = Operation::Clear}); }
esp_err_t Service::start_stream(const StreamCallbacks& callbacks)
{
    if (!callbacks.source && !callbacks.sink) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Stream, .stream = callbacks});
}
esp_err_t Service::set_volume(uint8_t percent)
{
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Volume, .value = percent});
}
esp_err_t Service::set_microphone_gain(float db)
{
    if (!std::isfinite(db) || db < 0 || db > 42) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Gain, .gain = db});
}
esp_err_t Service::set_format(uint32_t hz, uint8_t bits)
{
    if (!dsp::supported_rate(hz) || (bits != 16 && bits != 24)) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Format, .value = hz, .duration_ms = bits});
}
esp_err_t Service::set_sample_rate(uint32_t hz)
{
    if (!dsp::supported_rate(hz)) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Rate, .value = hz});
}
esp_err_t Service::set_bits_per_sample(uint8_t bits)
{
    if (bits != 16 && bits != 24) return ESP_ERR_INVALID_ARG;
    return enqueue({.operation = Operation::Bits, .value = bits});
}
void Service::stop()
{
    portENTER_CRITICAL(&context.gate);
    if (context.accepting) {
        context.published.stop_generation = context.epoch.fetch_add(1, std::memory_order_release) + 1;
        if (context.worker) xTaskNotifyGive(context.worker);
    }
    portEXIT_CRITICAL(&context.gate);
}
Snapshot Service::snapshot() const
{
    portENTER_CRITICAL(&context.gate);
    const Snapshot result = context.published;
    portEXIT_CRITICAL(&context.gate);
    return result;
}

} // namespace audio
