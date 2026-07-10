#pragma once

#include <memory>

class AppModule;

namespace app_modules {

std::unique_ptr<AppModule> createGyroReaderModule();

} // namespace app_modules
