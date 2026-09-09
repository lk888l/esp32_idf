# StickS3 audio service

`audio::Service` owns the ES8311 and one paired I2S TX/RX controller. The application initializes it after the board and shuts it down before the board. LVGL callbacks only enqueue commands or copy a snapshot; all codec/I2S/PMIC operations execute on the audio worker.

## Hardware path

- ES8311 control: shared BSP I2C bus, 7-bit address `0x18` (converted to the codec component's 8-bit address `0x30`).
- ESP32 pins: MCLK 18, BCLK 17, LRCK 15, TX 14, RX 16. These directions are relative to the ESP32; M5Unified confirms them.
- TX and RX are allocated together with `i2s_new_channel`, configured identically, and run together. ESP-IDF owns internal DMA buffers: six descriptors of 192 frames per direction. The application never bit-bangs audio.
- Mono 16-bit PCM uses 16-bit slots. Full 24-bit codec resolution uses signed 32-bit containers and 32-bit I2S slots, with the significant 24 bits left-aligned. MCLK is 256 × sample rate. This avoids unsupported 384 × sample-rate combinations in the ES8311 clock table at 24 and 44.1 kHz.
- AW8737 enable belongs to M5PM1 GPIO3, so the codec's `pa_pin` is `-1`. The BSP arbitrates speaker power against IR receive; conflicting starts report `ESP_ERR_INVALID_STATE` asynchronously.

Supported rates are 8000, 16000, 24000, 32000, 44100 and 48000 Hz. The default is 16000 Hz / 16 bit. Format changes are atomic through `set_format(rate, bits)` and clear the existing recording. The individual rate/bit-depth setters are also available.

## Menu-oriented operations

- `play_tone(frequency, duration)`: 20–4000 Hz, 100–30000 ms. The frequency must also be below Nyquist for the selected rate. A -12 dBFS sine has a 10 ms onset/offset envelope.
- `start_meter()`: continuous microphone RMS and peak in dBFS, plus clipped-sample count. These are digital levels, not calibrated sound-pressure measurements.
- `start_recording(duration)`: 100–30000 ms into PSRAM, with no internal-RAM fallback. Allocation failure is reported without aborting the application. At 48 kHz / 24 bit, 30 seconds requires 5.76 MB; availability depends on other PSRAM users.
- `play_recording()`, `clear_recording()`: replay or release the recorded clip. Stop and page navigation retain the clip; shutdown and format changes release it. Recordings and preferences are volatile.
- `set_volume(percent)`: 0–100 requested output level; default 30. Battery operation, or a failed power-status read, caps the effective level at 70. External input power permits 100. Active playback rechecks power once a second. Both requested and effective values appear in the snapshot.
- `set_microphone_gain(db)`: 0–42 dB, rounded down to the ES8311's 6 dB steps; default 24 dB.

A 50 ms settling interval is discarded before meter, recording, and capture-only stream data. Playback waits for the DMA ring to drain before disabling the amplifier so the tail is not cut off. Stop interrupts that drain.

## Application PCM integration

`start_stream(StreamCallbacks)` supports a PCM source, microphone sink, or both for full-duplex operation. This is the extension point for speech processing, file playback, network audio and recording storage; it requires no direct access to codec handles.

```cpp
audio::StreamCallbacks callbacks;
callbacks.context = &application_audio;
callbacks.source = [](void* user, void* pcm, size_t capacity) -> size_t {
    // Copy immediately available mono frames from an application-owned ring.
    // Return at most capacity; returning zero marks end-of-stream.
    return static_cast<ApplicationAudio*>(user)->read_pcm(pcm, capacity);
};
callbacks.sink = [](void* user, const void* pcm, size_t frames) -> esp_err_t {
    // Copy into an application-owned bounded ring, without blocking on disk
    // or network I/O. Report backpressure with a non-ESP_OK result.
    return static_cast<ApplicationAudio*>(user)->accept_microphone(pcm, frames);
};
esp_err_t queued = audio::Service::instance().start_stream(callbacks);
```

Callbacks execute on the worker and must return promptly (under 10 ms). They may copy buffers but cannot retain the supplied buffer pointers. They must not call lifecycle APIs. Source callbacks receive capacity in mono frames; 16-bit frames occupy two bytes, and 24-bit frames occupy four bytes. At most 192 frames are supplied per call. ESP-IDF full duplex shares clocks; it does not guarantee sample-exact TX/RX alignment. No acoustic echo cancellation is implied.

To release a callback context safely, first call `stop()`. Then poll a **single** snapshot until `settled_generation == stop_generation` and the state is `Idle` or `Fault`. Do not submit a new stream while releasing the previous context. An older `Idle` snapshot is insufficient because the worker may be opening a queued stream. A successful `deinitialize()` also joins the worker and makes releasing the context safe.

## Cancellation and recovery

The command queue contains eight entries. A command returns `ESP_OK` when accepted; `ESP_ERR_TIMEOUT` means the queue is full. Hardware completion and failures are reported by `snapshot()`. A new activity fully closes the previous activity before opening the next one. Volume and gain can change during an activity without interrupting it.

Stop uses a generation counter outside the queue, so it cannot be lost when the queue is full. Commands accepted before stop are invalidated and cannot restart audio. The worker publishes a settled generation only after it has stopped using the previous callback context. At most one control command runs between PCM chunks, preventing a stream of settings from starving DMA.

I2S reads and writes use a 50 ms timeout. The codec component bounds each I2C transfer at 100 ms; closing a faulty codec can require several transfers. `deinitialize()` waits up to three seconds and returns `ESP_ERR_TIMEOUT` if still stopping. On timeout, keep the board and callback context alive and retry `deinitialize()`; do not destroy dependencies. The worker is never forcibly deleted during hardware I/O. Cleanup errors retain remaining I2S/PA ownership for another cleanup attempt.

`rx_overruns` counts I2S receive queue overflow callbacks. `tx_underruns` counts failed/short bounded I2S writes; it is a software transport diagnostic, not a hardware underrun counter. Capture overruns can mean lost samples, even if the recording remains playable. Each new activity resets these activity diagnostics.

Any failed operation ends in `Fault` and preserves the error code. A later activity retries hardware setup from a clean state. The service uses no fatal `ESP_ERROR_CHECK` path and no device is required to boot the menu.

## Sources and validation boundary

The implementation uses Espressif's pinned `esp_codec_dev` 1.6.2 rather than a copied codec register demo:

- [M5Stack StickS3 hardware documentation](https://docs.m5stack.com/zh_CN/core/StickS3)
- [M5Unified official pin assignments](https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.cpp)
- [Espressif ES8311 I2S example](https://github.com/espressif/esp-idf/blob/v5.5.1/examples/peripherals/i2s/i2s_codec/i2s_es8311/main/i2s_es8311_example.c)
- [esp_codec_dev 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2)

The component has been compiled with the repository's ESP-IDF 5.5.4 Docker toolchain. Portable DSP helpers are separated in `audio_dsp.hpp` for host tests. Actual speaker loudness, microphone wiring/quality, clock accuracy, clipping thresholds under acoustic load and power transitions still require device acceptance testing.
