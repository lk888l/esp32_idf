/**
 * @file app_module.hpp
 * @brief Base class for application modules (C++)
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_MODULE_HPP
#define APP_MODULE_HPP

#include <string>


/**
 * @class AppModule
 * @brief Abstract base class for application modules
 * 
 * All application modules should inherit from this class to provide
 * a consistent interface for initialization, deinitialization, and processing.
 */
class AppModule {
public:
    /**
     * @brief Virtual destructor
     */
    virtual ~AppModule() = default;

    /**
     * @brief Initialize the module
     * @return true if successful, false otherwise
     */
    virtual bool initialize(void) = 0;

    /**
     * @brief Deinitialize the module
     * @return true if successful, false otherwise
     */
    virtual bool deinitialize(void) = 0;

    /**
     * @brief Check if module is initialized
     * @return true if initialized, false otherwise
     */
    virtual bool is_initialized(void) const = 0;

    /**
     * @brief Get the module name
     * @return Reference to module name string
     */
    virtual const std::string& get_name(void) const = 0;

    /**
     * @brief Process periodic module tasks
     */
    virtual void process(void) {}

protected:
    /**
     * @brief Protected constructor
     */
    AppModule() = default;

private:
    // Prevent copying
    AppModule(const AppModule&) = delete;
    AppModule& operator=(const AppModule&) = delete;
};

#endif /* APP_MODULE_HPP */
