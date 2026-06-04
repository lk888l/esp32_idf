/**
 * @file app_init.h
 * @brief Application initialization interface
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_INIT_H
#define APP_INIT_H

#include <esp_err.h>

/**
 * @brief Initialize application modules
 * @return ESP_OK if successful
 */
esp_err_t app_init(void);

/**
 * @brief Deinitialize application modules
 * @return ESP_OK if successful
 */
esp_err_t app_deinit(void);

#endif /* APP_INIT_H */
