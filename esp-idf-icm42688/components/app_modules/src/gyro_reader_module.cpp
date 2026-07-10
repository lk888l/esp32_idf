#include "gyro_reader_module.hpp"

#include <cstdio>
#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "app_task.hpp"
#include "bsp_board.hpp"
#include "bsp_i2c.hpp"
#include "esp_i2c_bus.hpp"
#include "icm42688/icm42688.hpp"
#include "logger.hpp"
#include "vqf.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app_modules {
namespace {

constexpr uint32_t kFusionIntervalMs = 10;
constexpr uint32_t kPrintIntervalMs = 500;
constexpr uint32_t kPrintEveryCycles = kPrintIntervalMs / kFusionIntervalMs;
constexpr uint32_t kGyroTaskStackSize = 4096;
constexpr UBaseType_t kGyroTaskPriority = tskIDLE_PRIORITY + 2;
constexpr float kGravityMps2 = 9.80665f;
constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kFusionTs = static_cast<float>(kFusionIntervalMs) / 1000.0f;

void delayMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

class GyroReaderTask final : public AppTask {
public:
    explicit GyroReaderTask(hardware::Icm42688& imu)
        : AppTask("gyro_reader", kGyroTaskStackSize, kGyroTaskPriority)
        , imu_(imu)
    {
    }

private:
    void main() override
    {
        ium::Logger log("gyro_reader");
        VQF vqf(kFusionTs);
        uint32_t cycles_until_print = 0;
        TickType_t last_wake_tick = xTaskGetTickCount();
        const TickType_t fusion_interval_ticks = pdMS_TO_TICKS(kFusionIntervalMs);

        log.info("gyro reader task started, VQF fusion interval={}ms", kFusionIntervalMs);

        while (!shouldExit()) {
            hardware::Icm42688Sample sample = {};
            const hardware::Icm42688Status status = imu_.readSample(sample);
            if (status == hardware::Icm42688Status::ok) {
                const vqf_real_t gyr[3] = {
                    static_cast<vqf_real_t>(sample.gyro_x_dps * kDegToRad),
                    static_cast<vqf_real_t>(sample.gyro_y_dps * kDegToRad),
                    static_cast<vqf_real_t>(sample.gyro_z_dps * kDegToRad),
                };
                const vqf_real_t acc[3] = {
                    static_cast<vqf_real_t>(sample.accel_x_g * kGravityMps2),
                    static_cast<vqf_real_t>(sample.accel_y_g * kGravityMps2),
                    static_cast<vqf_real_t>(sample.accel_z_g * kGravityMps2),
                };
                vqf.update(gyr, acc);

                // if (cycles_until_print == 0) {
                    vqf_real_t quat[4] = {};
                    vqf.getQuat6D(quat);

                    hardware::EulerAngles angles = {};
                    const hardware::Quaternion orientation{
                        static_cast<float>(quat[0]),
                        static_cast<float>(quat[1]),
                        static_cast<float>(quat[2]),
                        static_cast<float>(quat[3]),
                    };
                    const hardware::Icm42688Status angle_status =
                        hardware::quaternionToEulerAngles(orientation, angles);

                    if (angle_status == hardware::Icm42688Status::ok) {
                        printf("%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                               quat[0],
                               quat[1],
                               quat[2],
                               quat[3],
                               angles.roll_deg,
                               angles.pitch_deg,
                               angles.yaw_deg);
                    } else {
                        printf("%.4f,%.4f,%.4f,%.4f\n",
                               quat[0],
                               quat[1],
                               quat[2],
                               quat[3]);
                    }
                    cycles_until_print = kPrintEveryCycles > 0 ? kPrintEveryCycles - 1 : 0;
                // } else {
                //     --cycles_until_print;
                // }
            } else {
                log.warn("failed to read ICM42688 sample: {}", hardware::toString(status));
            }

            vTaskDelayUntil(&last_wake_tick, fusion_interval_ticks);
        }

        log.info("gyro reader task stopped");
    }

    hardware::Icm42688& imu_;
};

class GyroReaderModule final : public AppModule {
public:
    GyroReaderModule()
        : i2c_({bsp::kImuI2cPort, bsp::kImuI2cSda, bsp::kImuI2cScl})
    {
    }

    bool initialize() override
    {
        if (initialized_) {
            return true;
        }

        bsp::I2CStatus bus_status = i2c_.init();
        if (bus_status != bsp::I2CStatus::ok) {
            log_.error("failed to init I2C bus: {}", bsp::toString(bus_status));
            return false;
        }

        auto device_result = i2c_.createDevice(bsp::kIcm42688Address, bsp::kImuI2cClockHz);
        if (!device_result) {
            log_.error("failed to create ICM42688 I2C device: {}", bsp::toString(device_result.status));
            i2c_.deinit();
            return false;
        }
        imu_device_ = std::move(device_result.device);
        imu_ = std::make_unique<hardware::Icm42688>(*imu_device_,
                                                    hardware::Icm42688Config{.delay_ms = delayMs});
        task_ = std::make_unique<GyroReaderTask>(*imu_);

        const hardware::Icm42688Status imu_status = imu_->initialize();
        if (imu_status != hardware::Icm42688Status::ok) {
            log_.error("failed to init ICM42688: {}", hardware::toString(imu_status));
            task_.reset();
            imu_.reset();
            imu_device_.reset();
            i2c_.deinit();
            return false;
        }

        uint8_t who_am_i = 0;
        if (imu_->readWhoAmI(who_am_i) == hardware::Icm42688Status::ok) {
            log_.info("ICM42688 detected, WHO_AM_I=0x{:02X}", who_am_i);
        }

        if (!task_->start()) {
            log_.error("failed to start gyro reader task");
            task_.reset();
            imu_.reset();
            imu_device_.reset();
            i2c_.deinit();
            return false;
        }

        initialized_ = true;
        log_.info("initialized, I2C SDA={}, SCL={}, address=0x{:02X}",
                  static_cast<int>(bsp::kImuI2cSda),
                  static_cast<int>(bsp::kImuI2cScl),
                  bsp::kIcm42688Address);
        return true;
    }

    bool deinitialize() override
    {
        if (!initialized_) {
            return true;
        }

        bool ok = task_ == nullptr || task_->stop();
        task_.reset();
        imu_.reset();
        imu_device_.reset();
        ok = (i2c_.deinit() == bsp::I2CStatus::ok) && ok;
        initialized_ = !ok;
        return ok;
    }

    bool is_initialized() const override { return initialized_; }
    std::string_view name() const override { return "gyro_reader"; }

private:
    ium::Logger log_{"gyro_reader"};
    bsp::EspI2CBus i2c_;
    std::unique_ptr<bsp::I2CDevice> imu_device_;
    std::unique_ptr<hardware::Icm42688> imu_;
    std::unique_ptr<GyroReaderTask> task_;
    bool initialized_ = false;
};

} // namespace

std::unique_ptr<AppModule> createGyroReaderModule()
{
    return std::make_unique<GyroReaderModule>();
}

} // namespace app_modules
