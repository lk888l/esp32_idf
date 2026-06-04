/**
 * @file example_module.c
 * @brief Example module implementation template
 */

#include <string.h>
#include "esp_log.h"
#include "logger.h"
#include "example_module.h"

/* ----- Module Constants ----- */
static const char *TAG = "EXAMPLE_MODULE";
static const uint32_t DEFAULT_TIMEOUT = 5000;

/* ----- Module State ----- */
static struct {
    bool initialized;
    example_status_t status;
    example_config_t config;
    uint32_t process_count;
} module_state = {
    .initialized = false,
    .status = EXAMPLE_STATUS_IDLE,
    .config = {0},
    .process_count = 0
};

/* ----- Private Functions ----- */

/**
 * @brief Private helper function
 */
static esp_err_t priv_validate_config(const example_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Add validation logic */
    if (config->parameter1 == 0) {
        LOG_WARN(TAG, "Parameter1 is zero, using default");
    }
    
    return ESP_OK;
}

/* ----- Public Functions ----- */

esp_err_t example_init(const example_config_t *config)
{
    esp_err_t ret = ESP_OK;
    
    if (module_state.initialized) {
        LOG_WARN(TAG, "Module already initialized");
        return ESP_OK;
    }
    
    LOG_INFO(TAG, "Initializing module");
    
    /* Validate configuration */
    ret = priv_validate_config(config);
    if (ret != ESP_OK) {
        LOG_ERROR(TAG, "Configuration validation failed");
        return ret;
    }
    
    /* Copy configuration */
    memcpy(&module_state.config, config, sizeof(example_config_t));
    
    /* Initialize module-specific resources */
    module_state.status = EXAMPLE_STATUS_IDLE;
    module_state.process_count = 0;
    
    /* Mark as initialized */
    module_state.initialized = true;
    
    LOG_INFO(TAG, "Module initialized successfully");
    return ESP_OK;
}

esp_err_t example_deinit(void)
{
    if (!module_state.initialized) {
        LOG_WARN(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    LOG_INFO(TAG, "Deinitializing module");
    
    /* Cleanup resources */
    memset(&module_state, 0, sizeof(module_state));
    
    LOG_INFO(TAG, "Module deinitialized");
    return ESP_OK;
}

void example_process(void)
{
    if (!module_state.initialized) {
        return;
    }
    
    module_state.process_count++;
    
    /* Add periodic processing logic */
    if (module_state.process_count % 100 == 0) {
        LOG_DEBUG(TAG, "Processed %u cycles", module_state.process_count);
    }
}

example_status_t example_get_status(void)
{
    return module_state.status;
}

esp_err_t example_set_parameter(uint8_t param_id, uint32_t value)
{
    if (!module_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    LOG_DEBUG(TAG, "Setting parameter %u to %u", param_id, value);
    
    switch (param_id) {
        case 1:
            module_state.config.parameter1 = value;
            break;
        case 2:
            module_state.config.parameter2 = (uint16_t)value;
            break;
        default:
            LOG_WARN(TAG, "Unknown parameter id: %u", param_id);
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}
