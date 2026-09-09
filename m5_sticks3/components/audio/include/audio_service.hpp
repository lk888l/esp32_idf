#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace audio {

enum class State : uint8_t { Unavailable, Idle, Tone, Meter, Recording, Playback, Streaming, Fault };

struct Snapshot {
    bool initialized = false;
    bool recording_available = false;
    State state = State::Unavailable;
    uint32_t sample_rate_hz = 16000;
    uint8_t bits_per_sample = 16;
    uint8_t volume_percent = 30;
    uint8_t effective_volume_percent = 30;
    float microphone_gain_db = 24.0F;
    float rms_dbfs = -96.0F;
    float peak_dbfs = -96.0F;
    uint32_t recorded_ms = 0;
    uint32_t position_ms = 0;
    uint32_t duration_ms = 0;
    uint32_t clipped_samples = 0;
    uint32_t rx_overruns = 0;
    uint32_t tx_underruns = 0;
    uint32_t stop_generation = 0;
    uint32_t settled_generation = 0;
    esp_err_t last_error = ESP_OK;
};

// PCM is signed little-endian mono: int16_t for 16-bit, or int32_t with
// the 24 significant bits left-aligned for 24-bit. Frame counts are samples.
// Callbacks run on the audio worker; return promptly (< 10 ms), never call
// lifecycle APIs, and keep context valid until state is Idle/Fault AND
// snapshot().settled_generation == snapshot().stop_generation from one snapshot
// AFTER stop(), or deinitialize() has returned successfully. Buffers are valid
// only during each callback. A source returns 0 at EOF, at most capacity frames.
// A sink may be supplied alone (capture), or with a source (full duplex).
struct StreamCallbacks {
    void* context = nullptr;
    size_t (*source)(void* context, void* pcm, size_t capacity_frames) = nullptr;
    esp_err_t (*sink)(void* context, const void* pcm, size_t frames) = nullptr;
};

class Service {
public:
    static Service& instance();

    // Lifecycle calls belong to one application owner, outside the UI task.
    // Initialization creates the worker only; hardware is probed on first use.
    esp_err_t initialize();
    esp_err_t deinitialize();

    // Nonblocking bounded commands. ESP_OK means accepted, not completed.
    // A new activity stops the old one; failures are reported in snapshot().
    esp_err_t play_tone(uint32_t frequency_hz = 1000, uint32_t duration_ms = 2000);
    esp_err_t start_meter();
    esp_err_t start_recording(uint32_t duration_ms = 5000);
    esp_err_t play_recording();
    esp_err_t clear_recording();
    esp_err_t start_stream(const StreamCallbacks& callbacks);
    esp_err_t set_volume(uint8_t percent);
    esp_err_t set_microphone_gain(float db); // Quantized downward to 6 dB steps, 0..42.
    esp_err_t set_format(uint32_t hz, uint8_t bits); // Atomic format change; clears recording.
    esp_err_t set_sample_rate(uint32_t hz); // 8000/16000/24000/32000/44100/48000.
    esp_err_t set_bits_per_sample(uint8_t bits); // 16 or 24 (32-bit wire slots).

    // Stop invalidates all older queued commands. It never depends on queue
    // capacity. Playback/capture ends asynchronously; existing recording survives.
    void stop();
    Snapshot snapshot() const;

private:
    Service() = default;
};

} // namespace audio
