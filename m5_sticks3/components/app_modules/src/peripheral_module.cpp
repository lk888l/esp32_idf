#include "app_modules.hpp"

#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "audio_service.hpp"
#include "infrared_service.hpp"
#include "esp_log.h"

namespace app_modules {
namespace {

// One module for the board's mutually coordinated peripherals keeps lifecycle
// dependencies explicit and leaves a free slot in the fixed module registry.
class PeripheralModule final : public AppModule {
public:
    std::string_view name() const override { return "peripherals"; }

private:
    bool on_initialize() override
    {
        esp_err_t result = audio::Service::instance().initialize();
        if (result == ESP_OK) result = infrared::Service::instance().initialize();
        if (result != ESP_OK) {
            ESP_LOGE("peripherals", "service startup failed: %s", esp_err_to_name(result));
        }
        return result == ESP_OK;
    }

    bool on_deinitialize() override
    {
        // Both cleanup paths run even if one fails. AppModule retains failed
        // cleanup state so live tasks and their storage cannot be destroyed.
        const esp_err_t infrared_result = infrared::Service::instance().deinitialize();
        const esp_err_t audio_result = audio::Service::instance().deinitialize();
        if (infrared_result != ESP_OK || audio_result != ESP_OK) {
            ESP_LOGE("peripherals", "cleanup pending: IR=%s audio=%s",
                     esp_err_to_name(infrared_result), esp_err_to_name(audio_result));
        }
        return infrared_result == ESP_OK && audio_result == ESP_OK;
    }
};

} // namespace

std::unique_ptr<AppModule> create_peripheral_module()
{
    return std::make_unique<PeripheralModule>();
}

} // namespace app_modules
