#include "motion_fifo.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace app_modules::motion;

namespace {
std::array<uint8_t, 256> registers{};
std::vector<uint8_t> fifo_bytes;
bool fail_data = false, fail_flush = false;
unsigned data_reads = 0, flushes = 0;

void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void put(std::vector<uint8_t>& bytes, int16_t value)
{
    bytes.push_back(static_cast<uint16_t>(value) & 0xff);
    bytes.push_back(static_cast<uint16_t>(value) >> 8);
}

void frame(int16_t gx = 0, int16_t gy = 0, int16_t gz = 0,
           int16_t ax = 0, int16_t ay = 0, int16_t az = 8192)
{
    for (int16_t value : {gx, gy, gz, ax, ay, az}) put(fifo_bytes, value);
}

void reset_bus()
{
    registers.fill(0);
    registers[0x40] = 0xa9; registers[0x41] = 1;
    registers[0x42] = 0xe9; registers[0x43] = 1;
    fifo_bytes.clear(); fail_data = fail_flush = false;
    data_reads = flushes = 0;
}

void test_fifo()
{
    reset_bus();
    Bmi270Fifo fifo(nullptr);
    Bmi270Fifo::Batch batch;
    check(fifo.configure() == ESP_OK, "200 Hz FIFO configuration");
    check(registers[0x45] == 0x88 && registers[0x48] == 1 && registers[0x49] == 0xc0,
          "filtered, stop-on-full, headerless ACC+GYR FIFO");
    check(fifo.read_batch(batch) == ESP_OK && batch.count == 0 && data_reads == 0,
          "empty polls never generate samples");
    frame(32767, -32768, 4096, 8192, -4096, 0);
    frame(32767, -32768, 4096, 8192, -4096, 0);
    check(fifo.read_batch(batch) == ESP_OK && batch.count == 2 && fifo_bytes.empty(),
          "late poll consumes all real frames, including equal-valued samples");
    check(std::abs(batch.samples[0].gyro_dps[0] - 999.969482f) < 0.001f &&
          batch.samples[0].gyro_dps[1] == -1000 && batch.samples[0].gyro_dps[2] == 125,
          "gyro units, signed values and FIFO axis order");
    check(batch.samples[0].accel_g[0] == 1 && batch.samples[0].accel_g[1] == -0.5f,
          "accel scale and order");
    check(fifo.read_batch(batch) == ESP_OK && batch.count == 0, "consumed frames never repeated");

    fifo_bytes.push_back(1);
    const unsigned reads = data_reads;
    check(fifo.read_batch(batch) != ESP_OK && batch.count == 0 && data_reads == reads,
          "misaligned FIFO length rejected before data read");
    check(fifo.read_batch(batch) == ESP_OK && batch.count == 0 && fifo_bytes.empty(),
          "recover by flushing, without fabricated samples");

    for (size_t i = 0; i <= kMaxBatchFrames; ++i) frame();
    check(fifo.read_batch(batch) != ESP_OK && batch.count == 0, "stale/overflowing backlog rejected");
    check(fifo.read_batch(batch) == ESP_OK && fifo_bytes.empty(), "backlog discarded on recovery");

    frame(); frame(); fail_data = true;
    check(fifo.read_batch(batch) != ESP_OK && batch.count == 0, "partial I2C burst not integrated");
    fail_data = false; fail_flush = true;
    const unsigned before_retry = data_reads;
    check(fifo.read_batch(batch) != ESP_OK && batch.count == 0, "flush error reported");
    check(fifo.read_batch(batch) != ESP_OK && data_reads == before_retry,
          "failed flush cannot resume a misaligned FIFO");
    fail_flush = false;
    check(fifo.read_batch(batch) == ESP_OK && fifo_bytes.empty(), "flush retried successfully");
    frame();
    check(fifo.read_batch(batch) == ESP_OK && batch.count == 1, "fresh frames resume after recovery");

    frame(); frame(INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN);
    check(fifo.read_batch(batch) != ESP_OK && batch.count == 0, "overread sentinel rejects entire batch");
    check(fifo.resets() == 4 && fifo.max_batch_frames() == 2, "FIFO diagnostics count discontinuities");

    reset_bus(); registers[0x40] = 0xa8;
    Bmi270Fifo wrong_rate(nullptr);
    check(wrong_rate.configure() != ESP_OK, "mismatched sensor ODR cannot use fixed 5 ms integration");
    reset_bus(); registers[0x3c] = 0x7e; // -2, not +126.
    Bmi270Fifo calibrated(nullptr);
    check(calibrated.configure() == ESP_OK && calibrated.cross_axis() == -2,
          "factory cross-axis calibration sign extension");
    frame(0, 0, 16384);
    check(calibrated.read_batch(batch) == ESP_OK && batch.samples[0].gyro_dps[0] == 1.953125f,
          "factory Z-to-X compensation applied");
}

float angle_from(VQF& vqf, int axis)
{
    vqf_real_t q[4]; vqf.getQuat6D(q);
    float attitude[3];
    quaternion_to_euler(q, attitude[0], attitude[1], attitude[2]);
    float norm = 0;
    for (float value : q) norm += value * value;
    check(std::isfinite(norm) && std::abs(norm - 1) < 0.0001f, "finite, normalized quaternion");
    return attitude[axis];
}

void update_tilt(VQF& vqf, int axis, double angle, double rate)
{
    ImuSample sample;
    sample.gyro_dps[axis] = rate;
    sample.accel_g[axis == 0 ? 1 : 0] = std::sin(angle * kDegreesToRadians) * (axis == 0 ? 1 : -1);
    sample.accel_g[2] = std::cos(angle * kDegreesToRadians);
    update_fusion(vqf, sample);
}

float round_trip(double tau, int axis)
{
    auto parameters = fusion_parameters(); parameters.tauAcc = tau;
    VQF vqf(parameters, kSampleTimeSeconds);
    double previous = 0;
    float result = 0;
    constexpr double pi = 3.14159265358979323846;
    // 8 s rest, 0.3 s turn to 30 degrees, 1 s hold, 0.3 s return.
    // A 2% gyro sensitivity error + 0.3 deg/s bias exercise correction drift.
    for (int i = 0; i < 2120; ++i) {
        const double t = (i + 1) * .005;
        double angle = 0;
        if (t > 8 && t < 8.3) angle = 15 * (1 - std::cos(pi * (t - 8) / .3));
        else if (t >= 8.3 && t < 9.3) angle = 30;
        else if (t >= 9.3 && t < 9.6) angle = 15 * (1 + std::cos(pi * (t - 9.3) / .3));
        update_tilt(vqf, axis, angle, (angle - previous) / .005 * 1.02 + .3);
        previous = angle;
        result = angle_from(vqf, axis);
    }
    return std::abs(result);
}

void test_fusion()
{
    for (int axis : {0, 1}) {
        const float old_error = round_trip(2.0, axis);
        const float new_error = round_trip(fusion_parameters().tauAcc, axis);
        std::printf("axis=%d, return +1 s: tauAcc=2: %.4f deg, tauAcc=0.5: %.4f deg\n",
                    axis, old_error, new_error);
        check(new_error < 0.05f && new_error < old_error * .25f, "30-degree return correction tail reduced");

        reset_bus();
        Bmi270Fifo fifo(nullptr);
        Bmi270Fifo::Batch batch;
        check(fifo.configure() == ESP_OK, "batch integration FIFO setup");
        VQF batched(fusion_parameters(), kSampleTimeSeconds);
        // The mock sensor generates 3 s rest then 2 s at 30 deg/s. Variable
        // polling gaps must consume every quantized frame exactly once.
        int generated = 0, integrated = 0;
        while (generated < 1000) {
            const int count = std::min(1 + generated % 7, 1000 - generated);
            for (int j = 0; j < count; ++j, ++generated) {
                const double angle = generated < 600 ? 0 : (generated - 599) * .005 * 30;
                const int16_t rate = generated < 600 ? 0 : std::lround(30 * 32768 / 1000.0);
                const int16_t horizontal = std::lround(std::sin(angle * kDegreesToRadians) * 8192);
                const int16_t vertical = std::lround(std::cos(angle * kDegreesToRadians) * 8192);
                frame(axis == 0 ? rate : 0, axis == 1 ? rate : 0, 0,
                      axis == 1 ? -horizontal : 0, axis == 0 ? horizontal : 0, vertical);
            }
            check(fifo.read_batch(batch) == ESP_OK && batch.count == static_cast<size_t>(count),
                  "variable wake intervals preserve the sensor frame count");
            for (size_t n = 0; n < batch.count; ++n) {
                update_fusion(batched, batch.samples[n]); ++integrated;
            }
            check(fifo.read_batch(batch) == ESP_OK && batch.count == 0,
                  "poll between ODR ticks contributes no duplicate rotation");
        }
        check(integrated == 1000 && std::abs(angle_from(batched, axis) - 60) < .1f,
              "FIFO motion tracks 60 degrees without an output low-pass");

        VQF slow(fusion_parameters(), kSampleTimeSeconds);
        for (int n = 0; n < 1000; ++n) update_tilt(slow, axis, 0, 0);
        for (int n = 1; n <= 1000; ++n) update_tilt(slow, axis, n * .005 * .5, .5);
        check(angle_from(slow, axis) > 2.0f, "slow intentional tilts are not clamped to zero");
    }
}
} // namespace

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t* reg, size_t,
                                     uint8_t* output, size_t length, int)
{
    if (*reg == 0x24) {
        check(length == 2, "FIFO length read as one burst");
        output[0] = fifo_bytes.size() & 0xff; output[1] = fifo_bytes.size() >> 8;
    } else if (*reg == 0x26) {
        ++data_reads;
        check(length <= fifo_bytes.size(), "no FIFO overread");
        if (fail_data) {
            fifo_bytes.erase(fifo_bytes.begin(), fifo_bytes.begin() + 5);
            return ESP_FAIL;
        }
        std::memcpy(output, fifo_bytes.data(), length);
        fifo_bytes.erase(fifo_bytes.begin(), fifo_bytes.begin() + length);
    } else std::memcpy(output, registers.data() + *reg, length);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* data, size_t length, int)
{
    check(length == 2, "single-register writes");
    if (data[0] == 0x7e && data[1] == 0xb0) {
        if (fail_flush) return ESP_FAIL;
        ++flushes; fifo_bytes.clear();
    } else registers[data[0]] = data[1];
    return ESP_OK;
}

int main()
{
    test_fifo(); test_fusion();
    std::puts("Motion FIFO and VQF regression tests passed");
}
