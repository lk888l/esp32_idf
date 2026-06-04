/**
 * @file example_module.cpp
 * @brief Example application module implementation (C++)
 */

#include "example_module.hpp"
#include "logger.h"

namespace app {

    namespace {
        constexpr const char* TAG = "EXAMPLE_MODULE";
    }

    // Constructor
    ExampleModule::ExampleModule() : initialized_(false) {
        LOG_INFO(TAG, "ExampleModule created");
    }

    // Destructor
    ExampleModule::~ExampleModule() {
        if (initialized_) {
            deinitialize();
        }
        LOG_INFO(TAG, "ExampleModule destroyed");
    }

    bool ExampleModule::initialize(void) {
        if (initialized_) {
            LOG_WARN(TAG, "ExampleModule already initialized");
            return true;
        }

        try {
            LOG_INFO(TAG, "Initializing ExampleModule");
            
            // Add your initialization code here
            // Example: set up GPIO, initialize peripherals, etc.
            
            initialized_ = true;
            LOG_INFO(TAG, "ExampleModule initialized successfully");
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, "Initialization failed: %s", e.what());
            return false;
        }
    }

    bool ExampleModule::deinitialize(void) {
        if (!initialized_) {
            LOG_WARN(TAG, "ExampleModule not initialized");
            return true;
        }

        try {
            LOG_INFO(TAG, "Deinitializing ExampleModule");
            
            // Add your deinitialization code here
            // Example: cleanup GPIO, stop timers, etc.
            
            initialized_ = false;
            LOG_INFO(TAG, "ExampleModule deinitialized successfully");
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, "Deinitialization failed: %s", e.what());
            return false;
        }
    }

    bool ExampleModule::is_initialized(void) const {
        return initialized_;
    }

    const std::string& ExampleModule::get_name(void) const {
        return name_;
    }

    void ExampleModule::process(void) {
        ++process_count_;
        
        // Log every 100 cycles
        if (process_count_ % 100 == 0) {
            LOG_INFO(TAG, "ExampleModule processed: %u cycles", process_count_);
        }
        
        // Add your periodic processing code here
        // Example: read sensor values, update state, etc.
    }

    void ExampleModule::do_something(void) {
        if (!initialized_) {
            LOG_WARN(TAG, "ExampleModule not initialized");
            return;
        }
        
        LOG_INFO(TAG, "ExampleModule::do_something() called");
        // Add custom functionality here
    }

} // namespace app
