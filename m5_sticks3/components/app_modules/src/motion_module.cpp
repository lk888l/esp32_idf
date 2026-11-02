#include "app_modules.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "bmi270.h"
#include "bsp_board.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "motion_fifo.hpp"
#include "motion_runtime.hpp"
#include "motion_state.hpp"
#include "vqf.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr char kTag[] = "motion";
using namespace motion;
constexpr uint32_t kPollIntervalMs = 2;

std::atomic<bool> motion_requested{false};
std::atomic<MotionRuntimeState> runtime_state{MotionRuntimeState::stopped};

class MotionTask final : public AppTask {
public:
    explicit MotionTask(const Bmi270Fifo& fifo)
        : AppTask("vqf_fusion", 7168, tskIDLE_PRIORITY + 3, 1), fifo_(fifo)
    {
    }

private:
    void main() override
    {
        VQF vqf(fusion_parameters(), kSampleTimeSeconds);
        model::MotionSample published{};
        Bmi270Fifo::Batch batch;
        uint32_t failures = 0;
        int64_t last_sample = esp_timer_get_time();
#ifdef M5_STICKS3_MOTION_DIAGNOSTICS
        int64_t last_log = 0;
        ESP_LOGI("motion_diag", "us,samples,ax,ay,az,gx,gy,gz,roll,pitch,yaw,bx,by,bz,rest,fifo_resets,max_batch");
#endif
        ESP_LOGI(kTag, "VQF 6D: 200 Hz FIFO, tauAcc=0.5 s, gyro CAS=%d/512", fifo_.cross_axis());

        while (!should_exit()) {
            const esp_err_t result = fifo_.read_batch(batch);
            if (result == ESP_OK) {
                failures = 0;
                for (size_t frame = 0; frame < batch.count; ++frame) {
                    const ImuSample& sample = batch.samples[frame];
                    const float norm_squared = sample.accel_g[0] * sample.accel_g[0] +
                        sample.accel_g[1] * sample.accel_g[1] + sample.accel_g[2] * sample.accel_g[2];
                    if (!published.valid && norm_squared < 0.04f) continue;

                    // One update per real sensor frame, even when the task wakes
                    // late. Host polling intervals must not be used as VQF dt.
                    update_fusion(vqf, sample);
                    vqf_real_t quaternion[4]{};
                    vqf.getQuat6D(quaternion);
                    for (size_t axis = 0; axis < 3; ++axis) {
                        published.accel_g[axis] = sample.accel_g[axis];
                        published.gyro_dps[axis] = sample.gyro_dps[axis];
                    }
                    for (size_t index = 0; index < 4; ++index)
                        published.quaternion[index] = static_cast<float>(quaternion[index]);
                    quaternion_to_euler(published.quaternion, published.roll_deg,
                                        published.pitch_deg, published.yaw_deg);
                    published.rest_detected = vqf.getRestDetected();
                    published.valid = true;
                    ++published.sample_count;
                    last_sample = esp_timer_get_time();
                }
                // Publish the newest result, without queuing old poses for UI.
                if (batch.count && published.valid) {
                    model::MotionState::instance().publish(published);
#ifdef M5_STICKS3_MOTION_DIAGNOSTICS
                    if (last_sample - last_log >= 50'000) {
                        last_log = last_sample;
                        vqf_real_t bias[3];
                        vqf.getBiasEstimate(bias);
                        ESP_LOGI("motion_diag", "%lld,%lu,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%d,%lu,%u",
                            last_sample, published.sample_count,
                            published.accel_g[0], published.accel_g[1], published.accel_g[2],
                            published.gyro_dps[0], published.gyro_dps[1], published.gyro_dps[2],
                            published.roll_deg, published.pitch_deg, published.yaw_deg,
                            bias[0]*kRadiansToDegrees, bias[1]*kRadiansToDegrees, bias[2]*kRadiansToDegrees,
                            published.rest_detected, fifo_.resets(), static_cast<unsigned>(fifo_.max_batch_frames()));
                    }
#endif
                }
            } else {
                published.valid = false;
                model::MotionState::instance().publish(published);
                if (++failures == 1 || failures % 100 == 0)
                    ESP_LOGW(kTag, "BMI270 FIFO read failed: %s (count=%lu, resets=%lu)",
                             esp_err_to_name(result), failures, fifo_.resets());
            }
            if (published.valid && esp_timer_get_time() - last_sample > 100'000) {
                published.valid = false;
                model::MotionState::instance().publish(published);
                ESP_LOGW(kTag, "BMI270 sample timeout");
            }
            // Poll faster than ODR; an empty FIFO causes no fusion update.
            vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
        }
    }

    Bmi270Fifo fifo_;
};

class MotionModule final : public AppModule {
public:
    std::string_view name() const override { return "motion"; }

    void process() override
    {
        const bool requested = motion_requested.load(std::memory_order_acquire);
        const MotionRuntimeState state = runtime_state.load(std::memory_order_acquire);

        if (requested && state == MotionRuntimeState::stopped) {
            runtime_state.store(MotionRuntimeState::starting, std::memory_order_release);
            model::MotionState::instance().publish({});
            ESP_LOGI(kTag, "MOTION page opened; initializing BMI270");
            if (start_runtime()) {
                runtime_state.store(MotionRuntimeState::running, std::memory_order_release);
            } else {
                const bool cleaned = stop_runtime();
                runtime_state.store(MotionRuntimeState::failed, std::memory_order_release);
                if (!cleaned) {
                    ESP_LOGE(kTag, "motion startup failed and cleanup is incomplete");
                }
            }
            return;
        }

        if (!requested && state == MotionRuntimeState::running) {
            runtime_state.store(MotionRuntimeState::stopping, std::memory_order_release);
            if (stop_runtime()) {
                model::MotionState::instance().publish({});
                runtime_state.store(MotionRuntimeState::stopped, std::memory_order_release);
                ESP_LOGI(kTag, "MOTION page closed; BMI270 stopped and released");
            } else {
                runtime_state.store(MotionRuntimeState::failed, std::memory_order_release);
            }
            return;
        }

        if (!requested && state == MotionRuntimeState::failed && !sensor_ && !task_) {
            runtime_state.store(MotionRuntimeState::stopped, std::memory_order_release);
        }
    }

private:
    bool on_initialize() override
    {
        if (!bsp::Board::instance().initialized()) {
            ESP_LOGE(kTag, "board must be initialized before the motion module");
            return false;
        }

        motion_requested.store(false, std::memory_order_release);
#ifdef M5_STICKS3_MOTION_DIAGNOSTICS
        motion_requested.store(true, std::memory_order_release);
#endif
        runtime_state.store(MotionRuntimeState::stopped, std::memory_order_release);
        model::MotionState::instance().publish({});
#ifdef M5_STICKS3_MOTION_DIAGNOSTICS
        ESP_LOGI(kTag, "BMI270 diagnostics: automatic startup and 20 Hz serial output enabled");
#else
        ESP_LOGI(kTag, "BMI270 initialization deferred until the MOTION page opens");
#endif
        return true;
    }

    bool start_runtime()
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
            return false;
        }

        uint8_t chip_id = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_get_chip_id(sensor_, &chip_id));
        const bmi270_config_t sensor_config = {
            .acce_odr = BMI270_ACC_ODR_200_HZ,
            .acce_range = BMI270_ACC_RANGE_4_G,
            .gyro_odr = BMI270_GYR_ODR_200_HZ,
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
            return false;
        }
        result = bmi270_set_acce_bwp(sensor_, BMI270_ACC_BWP_NORM_AVG4);
        if (result == ESP_OK) result = bmi270_set_acce_filter_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED);
        if (result == ESP_OK) result = bmi270_set_gyro_bwp(sensor_, BMI270_GYR_BWP_NORM);
        if (result == ESP_OK) result = bmi270_set_gyro_noise_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED);
        if (result == ESP_OK) result = bmi270_set_gyro_filter_perf(sensor_, BMI270_PERFORMANCE_OPTIMIZED);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "BMI270 filter setup failed: %s", esp_err_to_name(result));
            return false;
        }
        // Gyroscope startup from suspend can take 80 ms. Discard that interval
        // before enabling FIFO so valid acceleration is not paired with zero gyro.
        vTaskDelay(pdMS_TO_TICKS(100));
        Bmi270Fifo fifo(sensor_->i2c_handle);
        result = fifo.configure();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "BMI270 FIFO setup failed: %s", esp_err_to_name(result));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        task_ = std::make_unique<MotionTask>(fifo);
        if (!task_->start()) {
            ESP_LOGE(kTag, "failed to start motion task");
            return false;
        }

        ESP_LOGI(kTag, "BMI270 ready: address=0x%02x chip_id=0x%02x", address, chip_id);
        return true;
    }

    bool on_deinitialize() override
    {
        motion_requested.store(false, std::memory_order_release);
        runtime_state.store(MotionRuntimeState::stopping, std::memory_order_release);
        const bool stopped = stop_runtime();
        model::MotionState::instance().publish({});
        runtime_state.store(stopped ? MotionRuntimeState::stopped
                                    : MotionRuntimeState::failed,
                            std::memory_order_release);
        return stopped;
    }

    bool stop_runtime()
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

void request_motion_enabled(bool enabled)
{
    const bool previous =
        motion_requested.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }

    model::MotionState::instance().publish({});
    ESP_LOGI(kTag, "motion runtime %s requested", enabled ? "start" : "stop");
}

bool motion_enabled_requested()
{
    return motion_requested.load(std::memory_order_acquire);
}

MotionRuntimeState motion_runtime_state()
{
    return runtime_state.load(std::memory_order_acquire);
}

std::unique_ptr<AppModule> create_motion_module()
{
    return std::make_unique<MotionModule>();
}

} // namespace app_modules
