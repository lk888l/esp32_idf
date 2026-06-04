/**
 * @file example_module.h
 * @brief Example module header template
 * @author Your Name
 * @date 2026-06-03
 * 
 * @details
 * This file demonstrates the proper structure for creating
 * a new module in this project.
 */

#ifndef EXAMPLE_MODULE_H
#define EXAMPLE_MODULE_H

#include <stdint.h>
#include <esp_err.h>

/* ----- Type Definitions ----- */

/**
 * @brief Example configuration structure
 */
typedef struct {
    uint32_t parameter1;
    uint16_t parameter2;
    uint8_t  flags;
} example_config_t;

/**
 * @brief Example status enumeration
 */
typedef enum {
    EXAMPLE_STATUS_IDLE = 0,
    EXAMPLE_STATUS_ACTIVE,
    EXAMPLE_STATUS_ERROR,
} example_status_t;

/* ----- Function Declarations ----- */

/**
 * @brief Initialize the example module
 * 
 * @param config Pointer to module configuration
 * @return ESP_OK on success, error code on failure
 * 
 * @note Must be called before using other module functions
 * @see example_deinit()
 */
esp_err_t example_init(const example_config_t *config);

/**
 * @brief Deinitialize the example module
 * 
 * @return ESP_OK on success, error code on failure
 * 
 * @note Cleans up resources allocated by example_init()
 */
esp_err_t example_deinit(void);

/**
 * @brief Process module periodic tasks
 * 
 * Call this function periodically to allow the module
 * to perform its internal processing.
 */
void example_process(void);

/**
 * @brief Get current module status
 * 
 * @return Current status of the module
 */
example_status_t example_get_status(void);

/**
 * @brief Set module parameter
 * 
 * @param param_id Parameter identifier
 * @param value New parameter value
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid parameter
 */
esp_err_t example_set_parameter(uint8_t param_id, uint32_t value);

#endif /* EXAMPLE_MODULE_H */
