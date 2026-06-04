/**
 * @file logger.h
 * @brief Unified logging interface
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_VERBOSE = 5,
} log_level_t;

/**
 * @brief Initialize logger
 * @param level Initial log level
 */
void logger_init(log_level_t level);

/**
 * @brief Set current log level
 * @param level Log level to set
 */
void logger_set_level(log_level_t level);

/**
 * @brief Get current log level
 * @return Current log level
 */
log_level_t logger_get_level(void);

/**
 * @brief Log a message at error level
 * @param tag Log tag
 * @param format Format string
 */
void log_error(const char *tag, const char *format, ...);

/**
 * @brief Log a message at warning level
 * @param tag Log tag
 * @param format Format string
 */
void log_warn(const char *tag, const char *format, ...);

/**
 * @brief Log a message at info level
 * @param tag Log tag
 * @param format Format string
 */
void log_info(const char *tag, const char *format, ...);

/**
 * @brief Log a message at debug level
 * @param tag Log tag
 * @param format Format string
 */
void log_debug(const char *tag, const char *format, ...);

/**
 * @brief Log a message at verbose level
 * @param tag Log tag
 * @param format Format string
 */
void log_verbose(const char *tag, const char *format, ...);

/* Convenience macros */
#define LOG_ERROR(tag, fmt, ...)   log_error(tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)    log_warn(tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)    log_info(tag, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(tag, fmt, ...)   log_debug(tag, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(tag, fmt, ...) log_verbose(tag, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
