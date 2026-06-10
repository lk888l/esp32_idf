/**
 * @file main.cpp
 * @brief Main application entry point (C++)
 * @author Your Name
 * @date 2026-06-03
 * 
 * @details
 * This is the main C++ application entry point. It initializes the system,
 * creates application tasks, and manages the overall application lifecycle.
 */

#include <stdio.h>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "project_config.h"
#include "app_init.hpp"
#include "app_tasks.hpp"
#include "logger.h"
#include "system.h"

namespace {
    /* ----- Constants ----- */
    constexpr const char* TAG = "MAIN";
    constexpr uint32_t MAIN_TASK_STACK_SIZE = 4096;
    constexpr UBaseType_t MAIN_TASK_PRIORITY = 5;

    /* ----- Function Declarations ----- */
    void main_task(void *pvParameter);
    esp_err_t system_startup(void);
}

/**
 * @brief Application main entry point
 * 
 * The FreeRTOS kernel calls this function once the microcontroller boots up.
 * All application initialization and task creation should happen here.
 */
extern "C" void app_main(void)
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
    LOG_INFO(TAG, "Application Starting (C++)...");
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
    {
        app::AppInit app_init_manager;
        if (!app_init_manager.initialize()) {
            LOG_ERROR(TAG, "Application initialization failed");
            esp_restart();
        }
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

namespace {
    /**
     * @brief Main application task
     * @param pvParameter Task parameter (unused)
     */
    void main_task(void *pvParameter)
    {
        (void)pvParameter;  /* Suppress unused parameter warning */
        TickType_t xLastWakeTime;
        const TickType_t xFrequency = pdMS_TO_TICKS(10);  /* 10 ms periodicity */
        xLastWakeTime = xTaskGetTickCount();
        LOG_INFO(TAG, "Main task started");

        // Create application task processor
        auto task_processor = std::make_unique<app::AppTasks>();

        while (1) {
            vTaskDelayUntil(&xLastWakeTime, xFrequency);
            task_processor->process();
        }
    }

    /**
     * @brief System startup initialization
     * @return ESP_OK on success
     */
    esp_err_t system_startup(void)
    {
        LOG_INFO(TAG, "Performing system startup");
        
        /* Add system startup procedures here */
        
        return ESP_OK;
    }
}
