/**
 * @file example_module.hpp
 * @brief Example application module (C++)
 * @author Your Name
 * @date 2026-06-03
 * 
 * This is an example of how to create a custom application module
 * by inheriting from AppModule base class.
 */

#ifndef EXAMPLE_MODULE_HPP
#define EXAMPLE_MODULE_HPP

#include <string>
#include <cstdint>
#include "app_module.hpp"

namespace app {

/**
 * @class ExampleModule
 * @brief Example module implementation
 * 
 * This module demonstrates the proper way to structure an application module.
 * It includes initialization, deinitialization, and periodic processing.
 */
class ExampleModule : public AppModule {
public:
    /**
     * @brief Constructor
     */
    ExampleModule();

    /**
     * @brief Destructor
     */
    ~ExampleModule() override;

    /**
     * @brief Initialize the module
     * @return true if successful, false otherwise
     */
    bool initialize(void) override;

    /**
     * @brief Deinitialize the module
     * @return true if successful, false otherwise
     */
    bool deinitialize(void) override;

    /**
     * @brief Check if module is initialized
     * @return true if initialized, false otherwise
     */
    bool is_initialized(void) const override;

    /**
     * @brief Get the module name
     * @return Reference to module name string
     */
    const std::string& get_name(void) const override;

    /**
     * @brief Process periodic module tasks
     */
    void process(void) override;

    /**
     * @brief Module-specific method
     */
    void do_something(void);

private:
    bool initialized_;                   ///< Initialization state
    std::string name_ = "ExampleModule"; ///< Module name
    uint32_t process_count_ = 0;         ///< Process counter
};

} // namespace app

#endif /* EXAMPLE_MODULE_HPP */
