#pragma once

#include <memory>

class AppModule;

namespace app_modules {

std::unique_ptr<AppModule> create_gpio_blink_module();

} // namespace app_modules
