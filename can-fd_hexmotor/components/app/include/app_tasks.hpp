/**
 * @file app_tasks.hpp
 * @brief Application tasks interface (C++)
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef APP_TASKS_HPP
#define APP_TASKS_HPP

#include <cstdint>


/**
 * @class AppTasks
 * @brief Application tasks processor
 * 
 * This class handles periodic application tasks that need to run during
 * the main application loop. Supports task counting and extensibility.
 */
class AppTasks {
public:
    /**
     * @brief Constructor
     */
    AppTasks();

    /**
     * @brief Destructor
     */
    ~AppTasks();

    /**
     * @brief Process application tasks
     * Called periodically from the main application loop
     */
    void process(void);

    /**
     * @brief Get the total number of processing cycles
     * @return Number of cycles completed
     */
    uint32_t get_cycle_count(void) const;

protected:
    /**
     * @brief Override this method to implement custom task processing
     * Called from process() after internal bookkeeping
     */
    virtual void process_tasks(void);

private:
    uint32_t call_count_;  ///< Number of process() calls
};

#endif /* APP_TASKS_HPP */

