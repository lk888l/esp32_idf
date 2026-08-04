#include "app_modules.hpp"

#include <memory>
#include <string_view>

#include "app_module.hpp"
#include "bsp_board.hpp"
#include "esp_err.h"
#include "esp_log.h"

namespace app_modules {
namespace {

class BoardModule final : public AppModule {
public:
    std::string_view name() const override { return "board"; }

private:
    bool on_initialize() override
    {
        const esp_err_t result = bsp::Board::instance().initialize();
        if (result != ESP_OK) {
            ESP_LOGE("board_module", "BSP initialization failed: %s", esp_err_to_name(result));
        }
        return result == ESP_OK;
    }

    bool on_deinitialize() override
    {
        const esp_err_t result = bsp::Board::instance().deinitialize();
        return result == ESP_OK;
    }
};

} // namespace

std::unique_ptr<AppModule> create_board_module()
{
    return std::make_unique<BoardModule>();
}

} // namespace app_modules
