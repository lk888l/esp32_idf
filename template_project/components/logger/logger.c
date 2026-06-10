/**
 * @file logger.c
 * @brief Unified logging implementation
 */

#include <stdio.h>
#include <stdarg.h>
#include "logger.h"

/* Log configuration */
static log_level_t current_level = LOG_LEVEL_INFO;
static const char *level_names[] = {
    "NONE",
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG",
    "VERBOSE"
};

/**
 * @brief Initialize logger
 */
void logger_init(log_level_t level)
{
    current_level = level;
}

/**
 * @brief Set current log level
 */
void logger_set_level(log_level_t level)
{
    current_level = level;
}

/**
 * @brief Get current log level
 */
log_level_t logger_get_level(void)
{
    return current_level;
}

/**
 * @brief Internal logging function
 */
static void logger_log(log_level_t level, const char *tag, const char *format, va_list args)
{
    if (level > current_level) {
        return;
    }

    printf("[%-7s] [%s] ", level_names[level], tag);
    vprintf(format, args);
    printf("\n");
}

/**
 * @brief Log error message
 */
void log_error(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    logger_log(LOG_LEVEL_ERROR, tag, format, args);
    va_end(args);
}

/**
 * @brief Log warning message
 */
void log_warn(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    logger_log(LOG_LEVEL_WARN, tag, format, args);
    va_end(args);
}

/**
 * @brief Log info message
 */
void log_info(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    logger_log(LOG_LEVEL_INFO, tag, format, args);
    va_end(args);
}

/**
 * @brief Log debug message
 */
void log_debug(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    logger_log(LOG_LEVEL_DEBUG, tag, format, args);
    va_end(args);
}

/**
 * @brief Log verbose message
 */
void log_verbose(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    logger_log(LOG_LEVEL_VERBOSE, tag, format, args);
    va_end(args);
}
