# ESP-IDF Professional Project Template

Professional-grade ESP-IDF project template with modern C/C++ standards, best practices, and modular architecture.

## 📋 Project Structure

```
sample_project/
├── main/                      # Main application component
│   ├── include/              # Public headers
│   │   ├── app_init.h
│   │   └── app_tasks.h
│   ├── app_init.c            # Application initialization
│   ├── app_tasks.c           # Application tasks
│   ├── main.c                # Entry point
│   └── CMakeLists.txt
├── components/
│   ├── logger/               # Logging component
│   │   ├── logger.h
│   │   ├── logger.c
│   │   └── CMakeLists.txt
│   └── system/               # System utilities component
│       ├── system.h
│       ├── system.c
│       └── CMakeLists.txt
├── config/                   # Project configuration
│   ├── project.mk
│   └── sdkconfig.defaults
├── docs/                     # Project documentation
├── tests/                    # Unit tests
├── tools/                    # Build and utility scripts
├── CMakeLists.txt           # Root CMake configuration
├── .gitignore
└── README.md

```

## ✨ Key Features

### Architecture
- **Modular Design**: Reusable components (logger, system utilities)
- **Layered Architecture**: Clear separation between application logic and system services
- **Scalable Structure**: Easy to add new components and features

### Code Quality
- **Modern C Standards**: C11 with strict compiler flags
- **Comprehensive Logging**: Unified logging system with multiple levels
- **Error Handling**: Proper ESP-IDF error code handling throughout
- **Documentation**: Doxygen-style comments on all public APIs

### Best Practices
- **Task Management**: Proper FreeRTOS task creation and management
- **Resource Initialization**: Orderly initialization with error checking
- **Memory Safety**: Stack overflow prevention, memory monitoring
- **Code Organization**: Logical separation of concerns

## 🚀 Getting Started

### Prerequisites
- ESP-IDF v5.0 or later
- CMake 3.16+
- Python 3.7+

### Building the Project

1. **Set environment variables**:
   ```bash
   export IDF_PATH=<path-to-esp-idf>
   export PATH="$IDF_PATH/tools:$PATH"
   ```

2. **Configure the project**:
   ```bash
   idf.py menuconfig
   ```

3. **Build the project**:
   ```bash
   idf.py build
   ```

4. **Flash the device**:
   ```bash
   idf.py -p /dev/ttyUSB0 flash
   ```

5. **Monitor output**:
   ```bash
   idf.py -p /dev/ttyUSB0 monitor
   ```

## 📚 Components

### Logger Component
Unified logging system with multiple severity levels:
- ERROR, WARN, INFO, DEBUG, VERBOSE
- Consistent formatting across application
- Easy to extend for file logging

**Usage**:
```c
LOG_INFO("MYTAG", "Application started: %s", version);
LOG_ERROR("MYTAG", "Error occurred: %s", esp_err_to_name(err));
```

### System Component
System utilities and diagnostics:
- Heap memory monitoring
- CPU information
- System uptime tracking
- Diagnostic printing

**Usage**:
```c
system_info_t info;
system_get_info(&info);
printf("Free heap: %u bytes\n", info.free_heap_size);
```

## 🔧 Development Workflow

### Adding a New Component

1. Create component directory:
   ```bash
   mkdir -p components/mycomponent
   ```

2. Create component files:
   - `mycomponent.h` - Public interface
   - `mycomponent.c` - Implementation
   - `CMakeLists.txt` - Build configuration

3. Update main component's CMakeLists.txt to depend on new component:
   ```cmake
   REQUIRES 
       mycomponent
   ```

### Logging in Your Code

```c
#include "logger.h"

static const char *TAG = "MY_MODULE";

void my_function(void) {
    LOG_DEBUG(TAG, "Function called");
    LOG_INFO(TAG, "Processing data: %d", value);
    LOG_ERROR(TAG, "Error occurred!");
}
```

## 🧪 Testing

Tests can be added to the `tests/` directory. Integration with CMake test framework allows running:

```bash
idf.py build
ctest
```

## 📝 Configuration

Project-wide settings are located in `config/`:
- `sdkconfig.defaults` - ESP-IDF build options
- `project.mk` - Project-specific variables

## 🐛 Debugging

Monitor application output:
```bash
idf.py monitor --raw
```

Enable debug logging:
```c
logger_set_level(LOG_LEVEL_DEBUG);
```

## 📦 Building for Different Targets

Switch target device:
```bash
idf.py set-target esp32s3
```

Supported targets: esp32, esp32s2, esp32s3, esp32c3, esp32c6, etc.

## 🤝 Contributing

When adding new features:
1. Follow the existing code style
2. Add Doxygen-style documentation
3. Include error handling
4. Add logging at appropriate levels
5. Update this README if needed

## 📄 License

[Add your license here]

## 📧 Support

For issues and questions, please refer to the [ESP-IDF documentation](https://docs.espressif.com/projects/esp-idf/).

---

**Last Updated**: 2026-06-03  
**Template Version**: 1.0.0  
**IDF Compatibility**: v5.0+
