/**
 * @file app_tasks.cpp
 * @brief Application tasks implementation (C++)
 */

#include "app_tasks.hpp"
#include "logger.h"

namespace app {

    namespace {
        constexpr const char* TAG = "APP_TASKS";
    }

    // Constructor
    AppTasks::AppTasks() : call_count_(0) {
        LOG_INFO(TAG, "AppTasks instance created");
    }

    // Destructor
    AppTasks::~AppTasks() {
        LOG_INFO(TAG, "AppTasks instance destroyed, total cycles: %u", call_count_);
    }

    void AppTasks::process(void) {
        /* Implement periodic application tasks here */
        ++call_count_;
        
        if (call_count_ % 100 == 0) {
            LOG_INFO(TAG, "Tasks processed: %u cycles", call_count_);
        }

        // Call user-defined task processing
        process_tasks();
    }

    void AppTasks::process_tasks(void) {
        // Override this method in derived classes to implement custom tasks
        // Example: read sensors, update state, etc.
    }

    uint32_t AppTasks::get_cycle_count(void) const {
        return call_count_;
    }

} // namespace app
