#pragma once

#include "app/app_module.hpp"
#include "app/app_task.hpp"
#include "canopen/standard_profile.hpp"
#include "canopen_esp32/esp_nvs_parameter_storage.hpp"
#include "canopen_esp32/esp_twai_transport.hpp"

#include <string_view>

namespace canopen_esp32 {

struct ModuleConfig {
    TwaiConfig twai{};
    canopen::ProfileConfig profile{};
    uint32_t task_stack_size = 6144;
    UBaseType_t task_priority = 12;
};

class CanopenModule final : public app::Module {
public:
    explicit CanopenModule(ModuleConfig config);
    bool initialize() override;
    bool deinitialize() override;
    void process() override;
    [[nodiscard]] bool initialized() const override { return initialized_; }
    [[nodiscard]] std::string_view name() const override { return "canopen"; }

private:
    static ModuleConfig prepare_config(ModuleConfig config,
                                       EspNvsParameterStorage& parameter_storage);

    class ServiceTask final : public app::Task {
    public:
        ServiceTask(EspTwaiTransport& transport,
                    canopen::StandardProfile& profile,
                    uint32_t stack_size,
                    UBaseType_t priority)
            : Task("canopen", stack_size, priority), transport_(transport), profile_(profile)
        {
        }

    private:
        void run() override;
        EspTwaiTransport& transport_;
        canopen::StandardProfile& profile_;
    };

    EspNvsParameterStorage parameter_storage_;
    ModuleConfig config_;
    EspTwaiTransport transport_;
    canopen::StandardProfile profile_;
    ServiceTask task_;
    bool initialized_ = false;
    uint32_t last_report_ms_ = 0;
};

} // namespace canopen_esp32

