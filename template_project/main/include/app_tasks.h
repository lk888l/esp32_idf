/**
 * @file app_tasks.h
 * @brief Application tasks interface
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <esp_err.h>

/**
 * @brief Process application tasks
 * Called periodically from the main application loop
 */
void app_tasks_process(void);

#endif /* APP_TASKS_H */
