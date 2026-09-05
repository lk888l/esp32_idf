#include "app_modules.hpp"
#include "app_module.hpp"
#include "connectivity_service.hpp"
#include "esp_log.h"

namespace app_modules {
namespace {
class ConnectivityModule final : public AppModule {
public:
    std::string_view name() const override { return "connectivity"; }
    void process() override { service_.process(); }
private:
    bool on_initialize() override
    {
        const esp_err_t result = service_.initialize();
        if (result != ESP_OK) ESP_LOGE("connectivity_module", "initialize: %s", esp_err_to_name(result));
        return result == ESP_OK;
    }
    bool on_deinitialize() override
    {
        const esp_err_t result = service_.deinitialize();
        if (result != ESP_OK) ESP_LOGE("connectivity_module", "cleanup: %s", esp_err_to_name(result));
        return result == ESP_OK;
    }
    connectivity::Service service_;
};
}
std::unique_ptr<AppModule> create_connectivity_module()
{
    return std::make_unique<ConnectivityModule>();
}
}
