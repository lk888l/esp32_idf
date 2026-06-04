/**
 * @file app_tasks.c
 * @brief Application tasks implementation
 */

#include "app_tasks.h"
#include "logger.h"

static const char *TAG = "APP_TASKS";
static unsigned int call_count = 0;

/**
 * @brief Process application tasks
 */
void app_tasks_process(void)
{
    /* Implement periodic application tasks here */
    if (++call_count % 100 == 0) {
        LOG_INFO(TAG, "Tasks processed: %u cycles", call_count);
    }
}
