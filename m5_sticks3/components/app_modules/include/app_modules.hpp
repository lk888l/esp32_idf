#pragma once

#include <memory>

class AppModule;

namespace app {
class ButtonEventBus;
}

namespace app_modules {

std::unique_ptr<AppModule> create_board_module();
std::unique_ptr<AppModule> create_motion_module();
std::unique_ptr<AppModule> create_wave_module();
std::unique_ptr<AppModule> create_ui_module(app::ButtonEventBus& event_bus);
std::unique_ptr<AppModule> create_button_module(app::ButtonEventBus& event_bus);

} // namespace app_modules
