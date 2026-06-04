/**
 * @file app.h
 * @brief Application public header
 * @author Your Name
 * @date 2026-06-03
 * 
 * This header provides the public application interface.
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <esp_err.h>

/**
 * @brief Get application build information
 * @return Build timestamp string
 */
const char* app_get_build_timestamp(void);

/**
 * @brief Get application version
 * @return Version string
 */
const char* app_get_version(void);

/**
 * @brief Get application uptime in milliseconds
 * @return Uptime in milliseconds
 */
uint64_t app_get_uptime_ms(void);

/**
 * @brief Request system restart
 * 
 * This function requests a graceful system restart.
 * The actual restart happens after all cleanup is performed.
 */
void app_request_restart(void);

#endif /* APP_H */
