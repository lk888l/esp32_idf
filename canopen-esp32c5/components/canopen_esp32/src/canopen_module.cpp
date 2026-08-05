#include "canopen_esp32/canopen_module.hpp"

#include "esp_log.h"
#include "esp_timer.h"

namespace canopen_esp32 {
namespace {
constexpr char kTag[] = "canopen";
}

CanopenModule::CanopenModule(ModuleConfig config)
    : parameter_storage_(config.profile.node_id)
    , config_(prepare_config(config, parameter_storage_))
    , transport_(config_.twai)
    , profile_(config_.profile, transport_)
    , task_(transport_, profile_, config_.task_stack_size, config_.task_priority)
{
}

ModuleConfig CanopenModule::prepare_config(ModuleConfig config,
                                           EspNvsParameterStorage& parameter_storage)
{
    config.profile.parameter_storage = &parameter_storage;
    if (parameter_storage.ready()) {
        config.profile.node_id = parameter_storage.startup_node_id();
    }
    return config;
}

bool CanopenModule::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!parameter_storage_.ready()) {
        ESP_LOGE(kTag, "persistent parameter storage is unavailable");
        return false;
    }
    if (!transport_.initialize()) {
        ESP_LOGE(kTag, "transport initialization failed");
        return false;
    }
    const canopen::AbortCode profile_result = profile_.initialize();
    if (profile_result != canopen::AbortCode::none) {
        ESP_LOGE(kTag,
                 "object dictionary initialization failed: 0x%08lx",
                 static_cast<unsigned long>(profile_result));
        (void)transport_.stop();
        return false;
    }
    if (!transport_.start()) {
        (void)transport_.stop();
        return false;
    }
    const uint64_t now_us = esp_timer_get_time();
    if (!profile_.start(now_us) || !task_.start()) {
        (void)transport_.stop();
        return false;
    }
    initialized_ = true;
    if (parameter_storage_.restored()) {
        ESP_LOGI(kTag, "restored node ID 0x%02X from NVS", config_.profile.node_id);
    }
    ESP_LOGI(kTag,
             "node 0x%02X ready, heartbeat=%ums, SDO RX/TX=0x%03X/0x%03X",
             config_.profile.node_id,
             config_.profile.producer_heartbeat_ms,
             canopen::cob::rsdo + config_.profile.node_id,
             canopen::cob::tsdo + config_.profile.node_id);
    return true;
}

bool CanopenModule::deinitialize()
{
    if (!initialized_) {
        return true;
    }
    const bool task_stopped = task_.stop(pdMS_TO_TICKS(2000));
    const bool transport_stopped = transport_.stop();
    initialized_ = !(task_stopped && transport_stopped);
    return !initialized_;
}

void CanopenModule::process()
{
    if (!initialized_) {
        return;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000U);
    if (now_ms - last_report_ms_ < 10'000U) {
        return;
    }
    last_report_ms_ = now_ms;
    const TwaiStatistics stats = transport_.statistics();
    ESP_LOGI(kTag,
             "state=0x%02X hb=%lu rx=%lu drop=%lu tx=%lu fail=%lu err=%lu recover=%lu",
             static_cast<unsigned>(profile_.node().state()),
             static_cast<unsigned long>(profile_.node().heartbeat_count()),
             static_cast<unsigned long>(stats.rx_frames),
             static_cast<unsigned long>(stats.rx_dropped),
             static_cast<unsigned long>(stats.tx_frames),
             static_cast<unsigned long>(stats.tx_failed),
             static_cast<unsigned long>(stats.bus_errors),
             static_cast<unsigned long>(stats.recoveries));
}

void CanopenModule::ServiceTask::run()
{
    while (!stop_requested()) {
        can::Frame frame{};
        if (transport_.receive(frame, 1)) {
            do {
                (void)profile_.handle(frame, esp_timer_get_time());
            } while (transport_.receive(frame, 0));
        }
        transport_.maintenance();
        profile_.process(esp_timer_get_time());
    }
}

} // namespace canopen_esp32

