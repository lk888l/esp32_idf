#include "app_modules.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "bmi270.h"
#include "bsp_board.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "motion_state.hpp"
#include "vqf.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr char kTag[] = "motion";
constexpr uint32_t kSampleIntervalMs = 10;
constexpr float kSampleTimeSeconds = 0.010f;
constexpr float kDegreesToRadians = 0.017453292519943295f;
constexpr float kRadiansToDegrees = 57.29577951308232f;
constexpr float kGravity = 9.80665f;

void quaternion_to_euler(const float quaternion[4], float& roll, float& pitch, float& yaw)
{
    const float w = quaternion[0];
    const float x = quaternion[1];
    const float y = quaternion[2];
    const float z = quaternion[3];

    roll = std::atan2(2.0f * (w * x + y * z),
                      1.0f - 2.0f * (x * x + y * y)) * kRadiansToDegrees;
    const float pitch_sine = std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
    pitch = std::asin(pitch_sine) * kRadiansToDegrees;
    yaw = std::atan2(2.0f * (w * z + x * y),
                     1.0f - 2.0f * (y * y + z * z)) * kRadiansToDegrees;
}

class MotionTask final : public AppTask {
public:
    explicit MotionTask(bmi270_handle_t* sensor)
        : AppTask("vqf_fusion", 7168, tskIDLE_PRIORITY + 3, 1), sensor_(sensor)
    {
    }

private:
    void main() override
    {
        VQFParams parameters;
        parameters.tauAcc = 2.0;
        parameters.restThGyr = 1.5;
        parameters.restThAcc = 0.08;
        VQF vqf(parameters, kSampleTimeSeconds);

        model::MotionSample published{};
        TickType_t last_wake = xTaskGetTickCount();
        uint32_t failures = 0;
        uint32_t warmup_reads = 0;
        ESP_LOGI(kTag, "VQF 6D fusion started at 100 Hz");

        while (!should_exit()) {
            float ax = 0.0f;
            float ay = 0.0f;
            float az = 0.0f;
            float gx = 0.0f;
            float gy = 0.0f;
            float gz = 0.0f;
            const esp_err_t accel_result = bmi270_get_acce_data(sensor_, &ax, &ay, &az);
            const esp_err_t gyro_result = bmi270_get_gyro_data(sensor_, &gx, &gy, &gz);

            const float accel_norm_squared = ax * ax + ay * ay + az * az;
            if (accel_result == ESP_OK && gyro_result == ESP_OK &&
                (published.valid || accel_norm_squared >= 0.04f)) {
                const vqf_real_t gyro_rad[3] = {
                    static_cast<vqf_real_t>(gx * kDegreesToRadians),
                    static_cast<vqf_real_t>(gy * kDegreesToRadians),
                    static_cast<vqf_real_t>(gz * kDegreesToRadians),
                };
                const vqf_real_t accel_mps2[3] = {
                    static_cast<vqf_real_t>(ax * kGravity),
                    static_cast<vqf_real_t>(ay * kGravity),
                    static_cast<vqf_real_t>(az * kGravity),
                };
                vqf.update(gyro_rad, accel_mps2);

                vqf_real_t quaternion[4]{};
                vqf.getQuat6D(quaternion);
                published.accel_g[0] = ax;
                published.accel_g[1] = ay;
                published.accel_g[2] = az;
                published.gyro_dps[0] = gx;
                published.gyro_dps[1] = gy;
                published.gyro_dps[2] = gz;
                for (size_t index = 0; index < 4; ++index) {
                    published.quaternion[index] = static_cast<float>(quaternion[index]);
                }
                quaternion_to_euler(published.quaternion,
                                    published.roll_deg,
                                    published.pitch_deg,
                                    published.yaw_deg);
                published.rest_detected = vqf.getRestDetected();
                published.valid = true;
                ++published.sample_count;
                model::MotionState::instance().publish(published);
                if (published.sample_count == 1) {
                    ESP_LOGI(kTag,
                             "first fused sample after %lu warm-up reads: "
                             "acc=[%.3f %.3f %.3f]g q=[%.3f %.3f %.3f %.3f]",
                             warmup_reads,
                             ax, ay, az, published.quaternion[0], published.quaternion[1],
                             published.quaternion[2], published.quaternion[3]);
                }
                failures = 0;
            } else if (accel_result == ESP_OK && gyro_result == ESP_OK) {
                ++warmup_reads;
                if (warmup_reads == 1) {
                    ESP_LOGI(kTag, "waiting for the first valid BMI270 sample");
                } else if (warmup_reads % 100 == 0) {
                    ESP_LOGW(kTag, "BMI270 still returning zero samples (count=%lu)", warmup_reads);
                }
            } else if (++failures == 1 || failures % 100 == 0) {
                ESP_LOGW(kTag, "BMI270 read failed (accel=%s gyro=%s, count=%lu)",
                         esp_err_to_name(accel_result), esp_err_to_name(gyro_result), failures);
            }

            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSampleIntervalMs));
        }
    }

    bmi270_handle_t* sensor_;
};

class MotionModule final : public AppModule {
public:
    std::string_view name() const override { return "motion"; }

private:
    bool on_initialize() override
    {
        if (!bsp::Board::instance().initialized()) {
            ESP_LOGE(kTag, "board must be initialized before the motion module");
            return false;
        }

        esp_err_t result = ESP_ERR_NOT_FOUND;
        uint8_t address = bsp::kBmi270Address;
        for (const uint8_t candidate : {BMI270_I2C_ADDRESS_L, BMI270_I2C_ADDRESS_H}) {
            const bmi270_driver_config_t driver_config = {
                .addr = candidate,
                .interface = BMI270_USE_I2C,
                .i2c_bus = bsp::Board::instance().i2c_bus(),
            };
            result = bmi270_create(&driver_config, &sensor_);
            if (result == ESP_OK) {
                address = candidate;
                break;
            }
            sensor_ = nullptr;
        }

        if (!sensor_) {
            ESP_LOGE(kTag, "BMI270 not found at 0x68/0x69: %s", esp_err_to_name(result));
            return true; // Keep the UI available and display the sensor error state.
        }

        uint8_t chip_id = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_get_chip_id(sensor_, &chip_id));
        const bmi270_config_t sensor_config = {
            .acce_odr = BMI270_ACC_ODR_100_HZ,
            .acce_range = BMI270_ACC_RANGE_4_G,
            .gyro_odr = BMI270_GYR_ODR_100_HZ,
            .gyro_range = BMI270_GYR_RANGE_1000_DPS,
        };
        result = bmi270_start(sensor_, &sensor_config);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "failed to start BMI270: %s", esp_err_to_name(result));
            const esp_err_t delete_result = bmi270_delete(sensor_);
            if (delete_result == ESP_OK) {
                sensor_ = nullptr;
            } else {
                ESP_LOGE(kTag, "failed to release inactive BMI270: %s",
                         esp_err_to_name(delete_result));
                return false;
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Wait for the first 100 Hz sample after power-on.
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_acce_bwp(sensor_, BMI270_ACC_BWP_NORM_AVG4));
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_acce_filter_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED));
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_gyro_bwp(sensor_, BMI270_GYR_BWP_NORM));
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_gyro_noise_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED));
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_gyro_filter_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED));

        task_ = std::make_unique<MotionTask>(sensor_);
        if (!task_->start()) {
            ESP_LOGE(kTag, "failed to start motion task");
            return false;
        }

        ESP_LOGI(kTag, "BMI270 ready: address=0x%02x chip_id=0x%02x", address, chip_id);
        return true;
    }

    bool on_deinitialize() override
    {
        if (task_ && !task_->stop()) {
            ESP_LOGE(kTag, "motion task did not stop before timeout");
            return false;
        }
        task_.reset();

        if (sensor_) {
            const esp_err_t stop_result = bmi270_stop(sensor_);
            if (stop_result != ESP_OK) {
                ESP_LOGW(kTag, "BMI270 stop returned %s; deleting the handle",
                         esp_err_to_name(stop_result));
            }
            const esp_err_t delete_result = bmi270_delete(sensor_);
            if (delete_result == ESP_OK) {
                sensor_ = nullptr;
            } else {
                ESP_LOGE(kTag, "failed to delete BMI270: %s", esp_err_to_name(delete_result));
                return false;
            }
        }
        return true;
    }

    bmi270_handle_t* sensor_ = nullptr;
    std::unique_ptr<MotionTask> task_;
};

} // namespace

std::unique_ptr<AppModule> create_motion_module()
{
    return std::make_unique<MotionModule>();
}

} // namespace app_modules
