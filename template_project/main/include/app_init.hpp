/**
 * @file app_init.hpp
 * @brief Application initialization interface (C++)
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_INIT_HPP
#define APP_INIT_HPP

#include <esp_err.h>

namespace app {

/**
 * @class AppInit
 * @brief Application initialization manager
 * 
 * This class handles all application-level initialization and deinitialization.
 * It uses RAII principles to ensure proper resource cleanup.
 */
class AppInit {
public:
    /**
     * @brief Constructor
     */
    AppInit();

    /**
     * @brief Destructor - automatically deinitializes if initialized
     */
    ~AppInit();

    /**
     * @brief Initialize application modules
     * @return true if successful, false otherwise
     */
    bool initialize(void);

    /**
     * @brief Deinitialize application modules
     * @return true if successful, false otherwise
     */
    bool deinitialize(void);

    /**
     * @brief Check if application is initialized
     * @return true if initialized, false otherwise
     */
    bool is_initialized(void) const;

private:
    bool initialized_;            ///< Initialization state
    bool logger_initialized_;     ///< Logger initialization state

    // Prevent copying
    AppInit(const AppInit&) = delete;
    AppInit& operator=(const AppInit&) = delete;
};

} // namespace app

#endif /* APP_INIT_HPP */
