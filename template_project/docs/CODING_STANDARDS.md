# Coding Standards Document

## C Code Standards

### 1. Naming Conventions

**Functions**:
- Use snake_case for function names
- Prefix component-specific functions: `component_function_name()`
- Example: `logger_init()`, `system_get_uptime_sec()`

**Variables**:
- Local variables: snake_case
- Static variables: prefix with `s_`
- Global variables: prefix with `g_`
- Constants: UPPER_SNAKE_CASE
- Example: `s_current_level`, `g_system_state`, `MAX_BUFFER_SIZE`

**Types**:
- Struct: snake_case with `_t` suffix
- Enum: snake_case with `_e` suffix or `_t` for enum types
- Example: `system_info_t`, `log_level_t`

### 2. File Organization

```c
// 1. Header guards
#ifndef COMPONENT_H
#define COMPONENT_H

// 2. Includes (standard, then system, then project)
#include <stdint.h>
#include "esp_err.h"
#include "logger.h"

// 3. Type definitions
typedef struct {
    // members
} component_t;

// 4. Function declarations
void component_init(void);

// 5. Inline functions (if any)

#endif /* COMPONENT_H */
```

### 3. Documentation

All public functions must include Doxygen-style documentation:

```c
/**
 * @brief Short description of the function
 * 
 * @param param1 Description of first parameter
 * @param param2 Description of second parameter
 * @return Description of return value
 * 
 * @note Any important notes
 * @warning Any warnings
 * @see Related functions
 */
void function_name(int param1, int param2);
```

### 4. Error Handling

- Always check return values from functions that can fail
- Use ESP-IDF error codes (esp_err_t)
- Use `ESP_ERROR_CHECK()` for critical errors
- Use `ESP_LOGI()` or logger macros for informational messages

```c
esp_err_t ret = some_function();
if (ret != ESP_OK) {
    LOG_ERROR(TAG, "Function failed: %s", esp_err_to_name(ret));
    return ret;
}
```

### 5. Code Style

**Indentation**: 4 spaces (no tabs)

**Braces**:
```c
if (condition) {
    // code
} else {
    // code
}

for (int i = 0; i < count; i++) {
    // code
}
```

**Comments**:
```c
// Use // for single-line comments
/* Use /* */ for multi-line comments */
```

**Line Length**: Aim for < 100 characters

**Pointer Declaration**:
```c
int *ptr;      // Correct
int* ptr;      // Avoid
int * ptr;     // Avoid
```

### 6. Constants and Macros

```c
/* Define related constants in a group */
static const uint32_t BUFFER_SIZE = 256;
static const uint16_t TIMEOUT_MS = 1000;

/* Use macros for compile-time configuration */
#define ENABLE_DEBUG 1
#define VERSION_MAJOR 1
#define VERSION_MINOR 0
```

### 7. Function Size and Complexity

- Keep functions small (< 50 lines ideally)
- Single responsibility principle
- Extract complex logic into helper functions

### 8. Memory Management

- Always check allocation results
- Use `free()` or component-specific deallocation
- Document memory ownership

```c
uint8_t *buffer = malloc(1024);
if (buffer == NULL) {
    return ESP_ERR_NO_MEM;
}
// Use buffer...
free(buffer);
```

### 9. Thread Safety

- Document which functions are thread-safe
- Use FreeRTOS synchronization primitives (mutex, semaphore)
- Add comments indicating critical sections

## Build Standards

### CMakeLists.txt

- Maintain consistent formatting
- Document non-obvious configurations
- Set appropriate compiler flags
- Separate public and private requirements

### Compilation Flags

Always enable:
```cmake
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wstrict-prototypes
)
```

## Git Commit Standards

Format: `<type>(<scope>): <subject>`

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `style`: Code style changes
- `refactor`: Code refactoring
- `test`: Test additions/modifications
- `chore`: Build system, dependencies

Example:
```
feat(logger): add file logging support

- Added logger_set_output_file() function
- Implemented log file rotation
- Updated tests
```

## Review Checklist

Before submitting code for review:

- [ ] Follows naming conventions
- [ ] Includes Doxygen documentation
- [ ] Handles errors properly
- [ ] No compiler warnings
- [ ] Code is well-commented
- [ ] Magic numbers are defined as constants
- [ ] No memory leaks
- [ ] Thread-safe where required
- [ ] Tested on target hardware
