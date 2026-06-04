/**
 * @file system.c
 * @brief System utilities implementation
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system.h"

static uint64_t start_time_ms = 0;

/**
 * @brief Initialize system components
 */
esp_err_t system_init(void)
{
    /* Record system start time */
    start_time_ms = esp_timer_get_time() / 1000;

    /* Print system information */
    system_print_diagnostics();

    return ESP_OK;
}

/**
 * @brief Get system information
 */
esp_err_t system_get_info(system_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(system_info_t));

    info->total_heap_size = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    info->free_heap_size = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    info->minimum_free_heap_size = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    info->cpu_count = SOC_CPU_CORES_NUM;
    info->cpu_freq = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    return ESP_OK;
}

/**
 * @brief Get system uptime in seconds
 */
uint64_t system_get_uptime_sec(void)
{
    return (esp_timer_get_time() / 1000 - start_time_ms) / 1000;
}

/**
 * @brief Print system diagnostics
 */
void system_print_diagnostics(void)
{
    system_info_t info;

    if (system_get_info(&info) == ESP_OK) {
        printf("\n");
        printf("========== System Information ==========\n");
        printf("CPU Count:           %u\n", info.cpu_count);
        printf("CPU Frequency:       %" PRIu32 " MHz\n", info.cpu_freq);
        printf("Total Heap Size:     %" PRIu32 " bytes\n", info.total_heap_size);
        printf("Free Heap Size:      %" PRIu32 " bytes\n", info.free_heap_size);
        printf("Minimum Free Heap:   %" PRIu32 " bytes\n", info.minimum_free_heap_size);
        printf("IDF Version:         %s\n", esp_get_idf_version());
        printf("========================================\n");
        printf("\n");
    }
}

/**
 * @brief Get application uptime in milliseconds
 * @return Uptime in milliseconds
 * 
 * @note This function is useful for system monitoring and diagnostics
 */
// uint64_t app_get_uptime_ms(void)
// {
//     return (uint64_t)esp_timer_get_time() / 1000;
// }
