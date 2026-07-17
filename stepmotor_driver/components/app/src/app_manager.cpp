#include "app_manager.hpp"

namespace app {

AppManager& AppManager::instance() noexcept
{
    static AppManager manager;
    return manager;
}

bool AppManager::registerModule(AppModule& module) noexcept
{
    if (started_ || module_count_ >= modules_.size()) {
        return false;
    }

    for (std::size_t index = 0; index < module_count_; ++index) {
        if (modules_[index] == &module) {
            return false;
        }
    }

    modules_[module_count_] = &module;
    ++module_count_;
    return true;
}

bool AppManager::initializeAll()
{
    if (started_) {
        return true;
    }

    std::size_t initialized_count = 0;
    for (; initialized_count < module_count_; ++initialized_count) {
        if (!modules_[initialized_count]->initialize()) {
            while (initialized_count > 0) {
                --initialized_count;
                modules_[initialized_count]->deinitialize();
            }
            return false;
        }
    }

    started_ = true;
    return true;
}

bool AppManager::deinitializeAll()
{
    bool success = true;
    for (std::size_t index = module_count_; index > 0; --index) {
        AppModule* module = modules_[index - 1];
        if (module->isInitialized() && !module->deinitialize()) {
            success = false;
        }
    }

    started_ = false;
    return success;
}

void AppManager::processAll()
{
    for (std::size_t index = 0; index < module_count_; ++index) {
        AppModule* module = modules_[index];
        if (module->isInitialized()) {
            module->process();
        }
    }
}

} // namespace app
