/**
 * @file main.c
 * @brief Main application entry point
 * @author Your Name
 * @date 2026-06-03
 * 
 * @details
 * This is the main application entry point. It initializes the system,
 * creates application tasks, and manages the overall application lifecycle.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "project_config.h"
#include "app_init.h"
#include "app_tasks.h"
#include "logger.h"
#include "system.h"


/* ----- Constants ----- */
static const char *TAG = "MAIN";
static const uint32_t MAIN_TASK_STACK_SIZE = 4096;
static const UBaseType_t MAIN_TASK_PRIORITY = 5;

/* ----- Function Declarations ----- */
static void main_task(void *pvParameter);
static esp_err_t system_startup(void);

/**
 * @brief Application main entry point
 * 
 * The FreeRTOS kernel calls this function once the microcontroller boots up.
 * All application initialization and task creation should happen here.
 */
void app_main(void)
{
    esp_err_t ret = ESP_OK;

    /* Initialize NVS (Non-Volatile Storage) */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize logger component */
    logger_init(LOG_LEVEL_INFO);
    LOG_INFO(TAG, "==============================================");
    LOG_INFO(TAG, "Application Starting...");
    LOG_INFO(TAG, "Project: %s v%s", PROJECT_NAME, PROJECT_VERSION);
    LOG_INFO(TAG, "Build Time: %s", BUILD_TIMESTAMP);
    LOG_INFO(TAG, "==============================================");

    /* Perform system startup procedures */
    ret = system_startup();
    if (ret != ESP_OK) {
        LOG_ERROR(TAG, "System startup failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* Initialize application modules */
    ret = app_init();
    if (ret != ESP_OK) {
        LOG_ERROR(TAG, "Application initialization failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* Create main application task */
    xTaskCreate(
        main_task,
        "main_task",
        MAIN_TASK_STACK_SIZE,
        NULL,
        MAIN_TASK_PRIORITY,
        NULL
    );

    LOG_INFO(TAG, "Application startup completed successfully");
}

/**
 * @brief Main application task
 * @param pvParameter Task parameter (unused)
 */
static void main_task(void *pvParameter)
{
    (void)pvParameter;  /* Suppress unused parameter warning */
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(10);  /* 10 ms periodicity */
    xLastWakeTime = xTaskGetTickCount();
    LOG_INFO(TAG, "Main task started");

    while (1) {
        /* Yield to other tasks */
        vTaskDelayUntil( &xLastWakeTime, xFrequency );

        /* Application main loop */
        app_tasks_process();
    }
    
    /* This line should never be reached */
    LOG_ERROR(TAG, "Main task loop exited unexpectedly");
    vTaskDelete(NULL);
}

/**
 * @brief System startup initialization
 * @return ESP_OK if successful, error code otherwise
 */
static esp_err_t system_startup(void)
{
    esp_err_t ret = ESP_OK;

    LOG_DEBUG(TAG, "Initializing system components");

    /* Initialize system utilities */
    ret = system_init();
    if (ret != ESP_OK) {
        LOG_ERROR(TAG, "System initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    LOG_DEBUG(TAG, "System startup completed");
    return ESP_OK;
}

