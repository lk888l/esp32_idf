/**
 * @file system.h
 * @brief System utilities and initialization
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef SYSTEM_H
#define SYSTEM_H

#include <esp_err.h>
#include <stdint.h>

/**
 * @brief System information structure
 */
typedef struct {
    uint32_t total_heap_size;
    uint32_t free_heap_size;
    uint32_t minimum_free_heap_size;
    uint8_t cpu_count;
    uint32_t cpu_freq;
} system_info_t;

/**
 * @brief Initialize system components
 * @return ESP_OK if successful
 */
esp_err_t system_init(void);

/**
 * @brief Get system information
 * @param info Pointer to system_info_t structure
 * @return ESP_OK if successful
 */
esp_err_t system_get_info(system_info_t *info);

/**
 * @brief Get system uptime in seconds
 * @return Uptime in seconds
 */
uint64_t system_get_uptime_sec(void);

/**
 * @brief Perform system diagnostics
 */
void system_print_diagnostics(void);

#endif /* SYSTEM_H */
