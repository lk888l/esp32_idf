/**
 * @file project_config.h
 * @brief Project configuration and version macros
 * @author Your Name
 * @date 2026-06-03
 */

#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

/* Project metadata - defined via CMake */
#ifndef PROJECT_NAME
#define PROJECT_NAME "sample_project"
#endif

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP __DATE__ " " __TIME__
#endif

#endif /* PROJECT_CONFIG_H */
