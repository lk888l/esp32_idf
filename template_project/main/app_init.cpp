/**
 * @file app_init.cpp
 * @brief Application initialization implementation (C++)
 */

#include "logger.h"
#include "app_init.hpp"

namespace app {

    namespace {
        constexpr const char* TAG = "APP_INIT";
    }

    // Constructor
    AppInit::AppInit() : initialized_(false) {
        LOG_INFO(TAG, "AppInit instance created");
    }

    // Destructor
    AppInit::~AppInit() {
        if (initialized_) {
            deinitialize();
        }
    }

    bool AppInit::initialize(void) {
        if (initialized_) {
            LOG_WARN(TAG, "AppInit already initialized");
            return true;
        }

        LOG_INFO(TAG, "Initializing application modules");

        // Initialize application-specific modules here
        initialized_ = true;
        LOG_INFO(TAG, "Application modules initialized successfully");
        return true;
    }

    bool AppInit::deinitialize(void) {
        if (!initialized_) {
            LOG_WARN(TAG, "AppInit not initialized");
            return true;
        }

        LOG_INFO(TAG, "Deinitializing application modules");

        // Deinitialize application-specific modules here

        initialized_ = false;
        LOG_INFO(TAG, "Application modules deinitialized successfully");
        return true;
    }

    bool AppInit::is_initialized(void) const {
        return initialized_;
    }

} // namespace app
