# Project Architecture

## Overview

This project follows a modular, layered architecture designed for maintainability, scalability, and reusability.

## Architecture Layers

```
┌─────────────────────────────────────┐
│   Application Logic (main, app_*)   │
├─────────────────────────────────────┤
│   Services & Components             │
│  (logger, system, device drivers)   │
├─────────────────────────────────────┤
│   FreeRTOS + ESP-IDF Framework      │
├─────────────────────────────────────┤
│   Hardware (Microcontroller)        │
└─────────────────────────────────────┘
```

## Component Structure

### Core Components

#### Logger Component
**Purpose**: Unified logging system
- Multi-level logging (ERROR, WARN, INFO, DEBUG, VERBOSE)
- Consistent message formatting
- Extensible to add file logging, remote logging, etc.

**Location**: `components/logger/`
**Public Interface**: `logger.h`

#### System Component
**Purpose**: System utilities and diagnostics
- Heap memory management information
- CPU statistics
- System uptime tracking
- Diagnostic output

**Location**: `components/system/`
**Public Interface**: `system.h`

### Application Component

**Main Entry Point** (`main.c`):
1. Initialize NVS (Non-Volatile Storage)
2. Initialize logger
3. Perform system startup
4. Initialize application
5. Create main application task

**Application Initialization** (`app_init.c`):
- Initialize application-specific modules
- Set up peripherals and drivers
- Configure communication interfaces

**Application Tasks** (`app_tasks.c`):
- Periodic application processing
- Event handling
- State machine execution

## Initialization Sequence

```
app_main()
  ├── nvs_flash_init()
  ├── logger_init()
  ├── system_startup()
  │   └── system_init()
  ├── app_init()
  │   └── [user-defined module initialization]
  └── xTaskCreate(main_task, ...)
      └── app_tasks_process() [periodic]
```

## Data Flow

```
User Events/Interrupts
    ↓
Main Task / Application Tasks
    ↓
Service Layer (System, Logger)
    ↓
ESP-IDF / FreeRTOS APIs
    ↓
Hardware
```

## Module Dependencies

```
main
├── app_init
├── app_tasks
├── logger
│   └── esp_log
└── system
    ├── esp_system
    └── freertos
```

## Adding New Components

To add a new component:

1. **Create component directory**:
   ```
   components/mycomponent/
   ├── mycomponent.h
   ├── mycomponent.c
   └── CMakeLists.txt
   ```

2. **Define public interface** (`mycomponent.h`):
   - Document all public functions
   - Use `_t` suffix for types
   - Clear parameter documentation

3. **Implement** (`mycomponent.c`):
   - Use consistent coding style
   - Add error checking
   - Include logging

4. **Configure build** (`CMakeLists.txt`):
   ```cmake
   idf_component_register(
       SRCS "mycomponent.c"
       INCLUDE_DIRS "."
       REQUIRES logger
   )
   ```

5. **Update main dependencies**:
   - Add to main's `CMakeLists.txt` REQUIRES
   - Include header in files that use it

## Communication Between Modules

**Recommended approaches** (in order of preference):

1. **Direct function calls** (for synchronous, simple operations)
   ```c
   esp_err_t status = logger_init(LOG_LEVEL_INFO);
   ```

2. **Callback functions** (for event notification)
   ```c
   void register_event_handler(event_handler_t handler);
   ```

3. **Message queues** (for asynchronous communication)
   ```c
   xQueueSend(event_queue, &event, portMAX_DELAY);
   ```

## Thread Safety

Components should document thread safety:
- **Thread-safe**: Multiple tasks can call concurrently
- **Not thread-safe**: Single-task use only
- **Partially thread-safe**: Specific functions are safe

## Configuration

Project-wide configuration:
- `config/sdkconfig.defaults` - ESP-IDF build options
- `config/project.mk` - Project settings

Component-specific configuration typically goes in:
- Component headers as `#define`
- CMakeLists.txt as build options
- FreeRTOS configuration

## Testing Strategy

1. **Unit tests**: Individual component testing
2. **Integration tests**: Component interaction testing
3. **System tests**: Full application testing

Tests located in `tests/` directory.

## Performance Considerations

- **Stack sizes**: Defined per task, monitor usage
- **Heap usage**: Use `system_get_info()` to monitor
- **Task priorities**: Document reasoning for priorities
- **Timing-critical code**: Minimize in interrupt handlers

## Debugging

Enable debug logging:
```c
logger_set_level(LOG_LEVEL_DEBUG);
```

Print system diagnostics:
```c
system_print_diagnostics();
```

Monitor real-time heap usage:
```c
system_info_t info;
system_get_info(&info);
```

## Version Management

- Major.Minor.Patch versioning
- Update `PROJECT_VERSION` in CMakeLists.txt
- Document breaking changes in version
