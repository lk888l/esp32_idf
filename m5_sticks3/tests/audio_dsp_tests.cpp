#include "audio_dsp.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define CHECK(expr) do { if (!(expr)) { \
 std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); \
} } while (0)

int main()
{
    using namespace audio::dsp;
    CHECK(supported_rate(8000) && supported_rate(16000) && supported_rate(24000));
    CHECK(supported_rate(32000) && supported_rate(44100) && supported_rate(48000));
    CHECK(!supported_rate(0) && !supported_rate(22050) && !supported_rate(UINT32_MAX));
    CHECK(sample_bytes(16) == 2 && sample_bytes(24) == 4);
    CHECK(quantize_gain(0) == 0 && quantize_gain(5.9F) == 0);
    CHECK(quantize_gain(6.0F) == 6 && quantize_gain(41.9F) == 36 && quantize_gain(42) == 42);
    const auto empty = measure(nullptr, 0, 16);
    CHECK(empty.rms_dbfs == -96 && empty.peak_dbfs == -96 && empty.clipped == 0);

    std::array<int16_t, 480> pcm{};
    auto levels = measure(pcm.data(), pcm.size(), 16);
    CHECK(levels.rms_dbfs == -96 && levels.peak_dbfs == -96);
    pcm.fill(16384);
    levels = measure(pcm.data(), pcm.size(), 16);
    CHECK(std::abs(levels.rms_dbfs + 6.0206F) < 0.0001F);
    CHECK(std::abs(levels.peak_dbfs + 6.0206F) < 0.0001F);
    CHECK(levels.clipped == 0);
    const int16_t extremes[] = {std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()};
    levels = measure(extremes, 2, 16);
    CHECK(levels.clipped == 2 && levels.peak_dbfs == 0);
    CHECK(levels.rms_dbfs > -0.001F);

    const int32_t highres[] = {std::numeric_limits<int32_t>::min(), 0x7FFFFF00};
    levels = measure(highres, 2, 24);
    CHECK(levels.clipped == 2 && levels.peak_dbfs == 0);
    CHECK(levels.rms_dbfs > -0.00001F);
    // Unaligned input is a legitimate source buffer and must not cause UB.
    std::array<uint8_t, 9> unaligned{};
    std::memcpy(unaligned.data() + 1, highres, sizeof(highres));
    CHECK(measure(unaligned.data() + 1, 2, 24).clipped == 2);
    CHECK(measure(highres, 2, 8).rms_dbfs == -96);

    for (const auto rate : {8000U, 16000U, 24000U, 32000U, 44100U, 48000U}) {
        for (const auto bits : {16U, 24U}) {
            constexpr size_t frames = 1000;
            std::array<uint8_t, frames * 4> contiguous{}, chunked{};
            tone(contiguous.data(), frames, bits, rate, 440, 0, frames);
            size_t cursor = 0;
            for (const size_t count : {1U, 191U, 17U, 192U, 127U, 472U}) {
                tone(chunked.data() + cursor * sample_bytes(bits), count, bits, rate, 440, cursor, frames);
                cursor += count;
            }
            CHECK(cursor == frames);
            CHECK(std::memcmp(contiguous.data(), chunked.data(), frames * sample_bytes(bits)) == 0);
            CHECK(normalized_sample(contiguous.data(), 0, bits) == 0);
            CHECK(normalized_sample(contiguous.data(), frames - 1, bits) == 0);
            levels = measure(contiguous.data(), frames, bits);
            CHECK(levels.clipped == 0 && levels.peak_dbfs <= -12.0F);
            if (bits == 24) for (size_t i = 0; i < frames; ++i) CHECK(contiguous[4 * i] == 0);
        }
    }
    // Steady-state RMS agrees with the analytic 0.25-amplitude sine wave.
    tone(pcm.data(), pcm.size(), 16, 48000, 1000, 4800, 48000);
    levels = measure(pcm.data(), pcm.size(), 16);
    CHECK(std::abs(levels.rms_dbfs + 15.0515F) < 0.005F);
    CHECK(std::abs(levels.peak_dbfs + 12.0412F) < 0.005F);
    std::puts("PCM format, clipping, dBFS, 24-bit padding and seamless chunked tone tests passed");
}
