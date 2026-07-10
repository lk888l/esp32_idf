#include "app_manager.hpp"

namespace app {

AppManager& AppManager::get_instance()
{
    static AppManager instance;
    return instance;
}

bool AppManager::register_module(std::unique_ptr<AppModule> module)
{
    if (!module || started_) {
        return false;
    }

    modules_.push_back(std::move(module));
    return true;
}

bool AppManager::initialize_all()
{
    if (started_) {
        return true;
    }

    std::size_t initialized_count = 0;
    for (auto& module : modules_) {
        if (!module || !module->initialize()) {
            while (initialized_count > 0) {
                --initialized_count;
                auto& initialized_module = modules_[initialized_count];
                if (initialized_module && initialized_module->is_initialized()) {
                    initialized_module->deinitialize();
                }
            }
            return false;
        }

        ++initialized_count;
    }

    started_ = true;
    return true;
}

bool AppManager::deinitialize_all()
{
    bool ok = true;

    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!*it) {
            ok = false;
            continue;
        }

        if ((*it)->is_initialized() && !(*it)->deinitialize()) {
            ok = false;
        }
    }

    if (ok) {
        started_ = false;
    }

    return ok;
}

void AppManager::process_all()
{
    for (auto& module : modules_) {
        if (module && module->is_initialized()) {
            module->process();
        }
    }
}

std::size_t AppManager::get_module_count() const
{
    return modules_.size();
}

AppModule* AppManager::get_module(std::size_t index)
{
    if (index >= modules_.size()) {
        return nullptr;
    }

    return modules_[index].get();
}

const AppModule* AppManager::get_module(std::size_t index) const
{
    if (index >= modules_.size()) {
        return nullptr;
    }

    return modules_[index].get();
}

bool AppManager::all_initialized() const
{
    for (const auto& module : modules_) {
        if (!module || !module->is_initialized()) {
            return false;
        }
    }

    return true;
}

} // namespace app
