/**
 * @file app_manager.hpp
 * @brief Application manager for coordinating all application modules (C++)
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_MANAGER_HPP
#define APP_MANAGER_HPP

#include <vector>
#include <memory>
#include <string>
#include "app_module.hpp"

namespace app {

/**
 * @class AppManager
 * @brief Central application manager
 * 
 * This singleton class manages all application modules and provides
 * a central point for application lifecycle management.
 */
class AppManager {
public:
    /**
     * @brief Get singleton instance
     * @return Reference to AppManager instance
     */
    static AppManager& get_instance(void);

    /**
     * @brief Register an application module
     * @param module Unique pointer to the module
     * @return true if registration successful
     */
    bool register_module(std::unique_ptr<AppModule> module);

    /**
     * @brief Initialize all registered modules
     * @return true if all modules initialized successfully
     */
    bool initialize_all(void);

    /**
     * @brief Deinitialize all registered modules (in reverse order)
     * @return true if all modules deinitialized successfully
     */
    bool deinitialize_all(void);

    /**
     * @brief Process all registered modules
     */
    void process_all(void);

    /**
     * @brief Get the number of registered modules
     * @return Number of modules
     */
    size_t get_module_count(void) const;

    /**
     * @brief Get module by index
     * @param index Module index
     * @return Pointer to module, or nullptr if not found
     */
    AppModule* get_module(size_t index);

    /**
     * @brief Check if all modules are initialized
     * @return true if all initialized, false otherwise
     */
    bool all_initialized(void) const;

private:
    /**
     * @brief Private constructor (singleton)
     */
    AppManager() = default;

    std::vector<std::unique_ptr<AppModule>> modules_;  ///< Registered modules

    // Prevent copying
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;
};

} // namespace app

#endif /* APP_MANAGER_HPP */
