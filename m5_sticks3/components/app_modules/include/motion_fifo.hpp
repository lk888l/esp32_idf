#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "motion_processing.hpp"

namespace app_modules::motion {

// Thin extension of the managed BMI270 driver: that driver does not expose FIFO
// or the factory cross-axis compensation. No changes to managed sources needed.
class Bmi270Fifo {
public:
    struct Batch {
        ImuSample samples[kMaxBatchFrames]{};
        size_t count = 0;
    };

    explicit Bmi270Fifo(i2c_master_dev_handle_t device) : device_(device) {}

    esp_err_t configure()
    {
        uint8_t config[4]{};
        esp_err_t result = read(0x40, config, sizeof(config));
        if (result != ESP_OK) return result;
        // 200 Hz, normal filters, performance mode, +/-4 g and +/-1000 dps.
        if (config[0] != 0xa9 || config[1] != 0x01 ||
            config[2] != 0xe9 || (config[3] & 0x07) != 0x01) return ESP_ERR_INVALID_STATE;
        result = write(0x2f, 0); // GYR_CAS is on feature page 0.
        if (result != ESP_OK) return result;
        uint8_t factor = 0;
        result = read(0x3c, &factor, 1);
        if (result != ESP_OK) return result;
        cross_axis_ = gyro_cross_axis_factor(factor);
        result = write(0x45, 0x88); // Filtered data, no FIFO downsampling.
        if (result != ESP_OK) return result;
        result = write(0x48, 0x01); // Stop on full, no sensortime trailer.
        if (result != ESP_OK) return result;
        result = write(0x49, 0xc0); // ACC+GYR, headerless, no AUX/tags.
        if (result != ESP_OK) return result;
        uint8_t fifo_config[2]{};
        result = read(0x48, fifo_config, sizeof(fifo_config));
        if (result != ESP_OK) return result;
        if (fifo_config[0] != 0x01 || fifo_config[1] != 0xc0) return ESP_ERR_INVALID_STATE;
        return write(0x7e, 0xb0); // Flush startup samples after settings settle.
    }

    esp_err_t read_batch(Batch& batch)
    {
        batch.count = 0;
        if (needs_flush_) {
            const esp_err_t result = write(0x7e, 0xb0);
            if (result == ESP_OK) needs_flush_ = false;
            return result;
        }
        uint8_t length[2]{};
        esp_err_t result = read(0x24, length, sizeof(length));
        if (result != ESP_OK) return result;
        const size_t bytes = length[0] | ((length[1] & 0x3fU) << 8);
        if (!bytes) return ESP_OK;
        // Never integrate a partial frame, an overflow, or an old backlog. An
        // I2C failure can consume part of a FIFO burst: flush before retrying.
        if (bytes % kFifoFrameBytes || bytes > sizeof(buffer_)) {
            invalidate();
            return ESP_ERR_INVALID_STATE;
        }
        result = read(0x26, buffer_, bytes);
        if (result != ESP_OK) {
            invalidate();
            return result;
        }
        const size_t frames = bytes / kFifoFrameBytes;
        for (size_t frame = 0; frame < frames; ++frame) {
            if (!decode_fifo_frame(buffer_ + frame * kFifoFrameBytes, cross_axis_, batch.samples[frame])) {
                invalidate();
                return ESP_ERR_INVALID_STATE;
            }
        }
        batch.count = frames;
        max_batch_frames_ = std::max(max_batch_frames_, frames);
        return ESP_OK;
    }

    int8_t cross_axis() const { return cross_axis_; }
    uint32_t resets() const { return resets_; }
    size_t max_batch_frames() const { return max_batch_frames_; }

private:
    esp_err_t read(uint8_t reg, uint8_t* data, size_t size)
    {
        return i2c_master_transmit_receive(device_, &reg, 1, data, size, 50);
    }

    esp_err_t write(uint8_t reg, uint8_t value)
    {
        const uint8_t data[]{reg, value};
        return i2c_master_transmit(device_, data, sizeof(data), 50);
    }

    void invalidate() { needs_flush_ = true; ++resets_; }

    i2c_master_dev_handle_t device_;
    uint8_t buffer_[kMaxBatchFrames * kFifoFrameBytes]{};
    int8_t cross_axis_ = 0;
    uint32_t resets_ = 0;
    size_t max_batch_frames_ = 0;
    bool needs_flush_ = false;
};

} // namespace app_modules::motion
