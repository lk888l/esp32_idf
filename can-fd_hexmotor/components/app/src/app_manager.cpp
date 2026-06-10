/**
 * @file app_manager.cpp
 * @brief Application manager implementation (C++)
 */

#include "app_manager.hpp"

namespace app {

    namespace {
        constexpr const char* TAG = "APP_MANAGER";
    }

    AppManager& AppManager::get_instance(void) {
        static AppManager instance;
        return instance;
    }

    bool AppManager::register_module(std::unique_ptr<AppModule> module) {
        if (!module) {
            // LOG_ERROR(TAG, "Cannot register nullptr module");
            return false;
        }

        const std::string& name = module->get_name();
        // LOG_INFO(TAG, "Registering module: %s", name.c_str());
        
        modules_.push_back(std::move(module));
        return true;
    }

    bool AppManager::initialize_all(void) {
        // LOG_INFO(TAG, "Initializing %u modules", modules_.size());
        
        for (size_t i = 0; i < modules_.size(); ++i) {
            const std::string& name = modules_[i]->get_name();
            if (!modules_[i]->initialize()) {
                // LOG_ERROR(TAG, "Failed to initialize module: %s", name.c_str());
                return false;
            }
            // LOG_INFO(TAG, "Module initialized: %s [%u/%u]", name.c_str(), i + 1, modules_.size());
        }

        // LOG_INFO(TAG, "All modules initialized successfully");
        return true;
    }

    bool AppManager::deinitialize_all(void) {
        // LOG_INFO(TAG, "Deinitializing %u modules (reverse order)", modules_.size());
        
        // Deinitialize in reverse order (LIFO)
        for (int i = modules_.size() - 1; i >= 0; --i) {
            const std::string& name = modules_[i]->get_name();
            if (!modules_[i]->deinitialize()) {
                // LOG_ERROR(TAG, "Failed to deinitialize module: %s", name.c_str());
                return false;
            }
            // LOG_INFO(TAG, "Module deinitialized: %s", name.c_str());
        }

        // LOG_INFO(TAG, "All modules deinitialized successfully");
        return true;
    }

    void AppManager::process_all(void) {
        for (auto& module : modules_) {
            if (module && module->is_initialized()) {
                module->process();
            }
        }
    }

    size_t AppManager::get_module_count(void) const {
        return modules_.size();
    }

    AppModule* AppManager::get_module(size_t index) {
        if (index >= modules_.size()) {
            return nullptr;
        }
        return modules_[index].get();
    }

    bool AppManager::all_initialized(void) const {
        for (const auto& module : modules_) {
            if (!module || !module->is_initialized()) {
                return false;
            }
        }
        return true;
    }

} // namespace app
