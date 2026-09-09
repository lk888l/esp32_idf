#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audio::dsp {

inline constexpr bool supported_rate(uint32_t hz)
{
    return hz == 8000 || hz == 16000 || hz == 24000 || hz == 32000 || hz == 44100 || hz == 48000;
}

inline constexpr size_t sample_bytes(uint8_t bits) { return bits == 16 ? 2 : 4; }
inline constexpr float quantize_gain(float db) { return static_cast<int>(db / 6.0F) * 6.0F; }

struct Levels {
    float rms_dbfs = -96.0F;
    float peak_dbfs = -96.0F;
    uint32_t clipped = 0;
};

inline float dbfs(double linear)
{
    return linear > 0.0 ? static_cast<float>(std::max(-96.0, 20.0 * std::log10(linear))) : -96.0F;
}

inline double normalized_sample(const void* pcm, size_t index, uint8_t bits)
{
    if (bits == 16) {
        int16_t value;
        std::memcpy(&value, static_cast<const uint8_t*>(pcm) + index * 2, 2);
        return static_cast<double>(value) / 32768.0;
    }
    int32_t value;
    std::memcpy(&value, static_cast<const uint8_t*>(pcm) + index * 4, 4);
    return static_cast<double>(value) / 2147483648.0;
}

inline Levels measure(const void* pcm, size_t frames, uint8_t bits)
{
    Levels result;
    if (!pcm || !frames || (bits != 16 && bits != 24)) return result;
    double sum = 0.0, peak = 0.0;
    const double clip_threshold = bits == 16 ? 32767.0 / 32768.0 : 8388607.0 / 8388608.0;
    for (size_t i = 0; i < frames; ++i) {
        const double value = normalized_sample(pcm, i, bits);
        const double magnitude = std::abs(value);
        sum += value * value;
        peak = std::max(peak, magnitude);
        result.clipped += magnitude >= clip_threshold;
    }
    result.rms_dbfs = dbfs(std::sqrt(sum / frames));
    result.peak_dbfs = dbfs(peak);
    return result;
}

// Phase comes from absolute frame position, so chunk boundaries never reset it.
// A 10 ms envelope avoids tone onset/offset clicks; amplitude is -12 dBFS.
inline void tone(void* pcm, size_t frames, uint8_t bits, uint32_t rate,
                 uint32_t frequency, uint64_t offset, uint64_t total)
{
    const uint64_t ramp = std::max<uint32_t>(1, rate / 100);
    constexpr double pi = 3.14159265358979323846;
    for (size_t i = 0; i < frames; ++i) {
        const uint64_t position = offset + i;
        const uint64_t remaining = total > position ? total - position - 1 : 0;
        const double envelope = static_cast<double>(std::min({ramp, position, remaining})) / ramp;
        const double phase = 2.0 * pi * ((position * frequency) % rate) / rate;
        const double value = std::sin(phase) * envelope * 0.25;
        if (bits == 16) {
            const int16_t sample = static_cast<int16_t>(value * 32767.0);
            std::memcpy(static_cast<uint8_t*>(pcm) + 2 * i, &sample, 2);
        } else {
            const int32_t sample = static_cast<int32_t>(value * 8388607.0) * 256;
            std::memcpy(static_cast<uint8_t*>(pcm) + 4 * i, &sample, 4);
        }
    }
}

} // namespace audio::dsp
