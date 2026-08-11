#include "app_modules.hpp"

#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "wave_generator.hpp"

namespace app_modules {
namespace {

constexpr char kTag[] = "wave_module";

class WaveModule final : public AppModule {
public:
    std::string_view name() const override { return "wave_generator"; }

private:
    bool on_initialize() override
    {
        const esp_err_t result = wave::Generator::instance().initialize();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "wave generator initialization failed: %s",
                     esp_err_to_name(result));
        }
        return result == ESP_OK;
    }

    bool on_deinitialize() override
    {
        const esp_err_t result = wave::Generator::instance().deinitialize();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "wave generator cleanup failed: %s", esp_err_to_name(result));
        }
        return result == ESP_OK;
    }
};

} // namespace

std::unique_ptr<AppModule> create_wave_module()
{
    return std::make_unique<WaveModule>();
}

} // namespace app_modules
