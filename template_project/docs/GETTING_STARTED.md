# Getting Started Guide

## Prerequisites

Before you start, ensure you have:

- **ESP-IDF**: v5.0 or later
- **CMake**: 3.16 or later  
- **Python**: 3.7 or later
- **Git**: For version control
- **C/C++ Compiler**: (included with ESP-IDF tools)

## Installation

### 1. Install ESP-IDF

On Linux/macOS:
```bash
mkdir -p ~/esp
cd ~/esp
git clone https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.1  # or latest stable version
./install.sh
```

On Windows (PowerShell):
```powershell
mkdir $env:USERPROFILE\esp
cd $env:USERPROFILE\esp
git clone https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.1
.\install.bat
```

### 2. Set Environment Variables

On Linux/macOS:
```bash
export IDF_PATH=~/esp/esp-idf
export PATH="$IDF_PATH/tools:$PATH"
```

On Windows (PowerShell):
```powershell
$env:IDF_PATH = "$env:USERPROFILE\esp\esp-idf"
$env:PATH += ";$env:IDF_PATH\tools"
```

### 3. Verify Installation

```bash
idf.py --version
python -m pip show esptool
```

## Project Setup

### 1. Clone/Obtain Project

```bash
cd ~/projects
git clone <your-project-url>
cd sample_project
```

### 2. Set Target Device (Optional)

Default is ESP32. To use a different target:

```bash
idf.py set-target esp32s3
```

Available targets:
- `esp32` - Classic ESP32
- `esp32s2` - ESP32-S2
- `esp32s3` - ESP32-S3
- `esp32c3` - ESP32-C3
- `esp32c6` - ESP32-C6

## Building

### Clean Build

```bash
idf.py build
```

### Full Clean and Build

```bash
idf.py fullclean
idf.py build
```

### Build Specific Components

```bash
idf.py build app
idf.py build logger
idf.py build system
```

## Configuring the Project

### Using menuconfig

For interactive configuration:

```bash
idf.py menuconfig
```

Key sections to configure:
- **Serial Flasher Config** → Flash size, mode, frequency
- **Partition Table** → Adjust if needed
- **Component config** → Device-specific settings

Configuration is saved in `sdkconfig` file.

### Manual Configuration

Edit `config/sdkconfig.defaults` for default settings.

## Flashing the Device

### 1. Connect Device

Connect your ESP32 board via USB. Identify the port:

**Linux/macOS**:
```bash
ls /dev/ttyUSB*    # or /dev/cu.usbserial-*
```

**Windows** (PowerShell):
```powershell
Get-CimInstance -ClassName Win32_SerialPort
```

### 2. Flash the Firmware

```bash
idf.py -p /dev/ttyUSB0 flash
```

Or use the provided script:

```bash
./tools/flash.sh /dev/ttyUSB0
```

### 3. Monitor Serial Output

After flashing:

```bash
idf.py -p /dev/ttyUSB0 monitor
```

You should see:
```
[MAIN] Application Starting...
[MAIN] Project: sample_project v1.0.0
[SYSTEM] ========== System Information ==========
[SYSTEM] CPU Count: 2
[SYSTEM] CPU Frequency: 240 MHz
```

Exit monitor: Press `Ctrl+]`

## Common Operations

### One-Command Build and Flash

```bash
idf.py -p /dev/ttyUSB0 build flash monitor
```

Or use:
```bash
idf.py -p /dev/ttyUSB0 buildFlashMonitor
```

### Erase Flash Memory

```bash
idf.py -p /dev/ttyUSB0 erase_flash
```

### View Project Configuration

```bash
idf.py show_efuse
idf.py version
```

### Size Analysis

```bash
idf.py size
idf.py size-components
```

## Modifying the Application

### 1. Application Entry Point

File: `main/main.c`

The `app_main()` function is the entry point. Here you can:
- Initialize components
- Create tasks
- Set up event handlers

### 2. Adding Application Logic

File: `main/app_init.c` and `main/app_tasks.c`

**In `app_init()`**:
- Initialize your modules
- Configure peripherals
- Set up state

**In `app_tasks_process()`**:
- Implement periodic processing
- Handle state machines
- Process data

### 3. Using the Logger

```c
#include "logger.h"

static const char *TAG = "MY_MODULE";

void my_function(void) {
    LOG_INFO(TAG, "Function started");
    
    if (error) {
        LOG_ERROR(TAG, "An error occurred: %d", error_code);
    }
    
    LOG_DEBUG(TAG, "Debug info: %s", data);
}
```

### 4. Using System Utilities

```c
#include "system.h"

system_info_t info;
system_get_info(&info);
printf("Free memory: %u bytes\n", info.free_heap_size);
```

## Creating New Components

### Example: Adding a WiFi Component

1. Create directory:
   ```bash
   mkdir -p components/wifi_manager
   ```

2. Create files:
   - `components/wifi_manager/wifi_manager.h`
   - `components/wifi_manager/wifi_manager.c`
   - `components/wifi_manager/CMakeLists.txt`

3. Update main's `CMakeLists.txt`:
   ```cmake
   idf_component_register(
       REQUIRES
           wifi_manager
           logger
   )
   ```

4. Use in application:
   ```c
   #include "wifi_manager.h"
   
   wifi_manager_init();
   ```

## Troubleshooting

### Port Not Found

**Problem**: `idf.py: error: argument -p/--port: ... does not exist`

**Solution**:
- Check USB cable connection
- Verify device drivers installed
- Use `idf.py list-ports` to show available ports

### Build Fails with Memory Error

**Problem**: Out of memory during build

**Solution**:
```bash
idf.py clean
idf.py build -j 1  # Reduce parallel jobs
```

### Garbage Output on Monitor

**Problem**: Garbled characters in serial monitor

**Solution**:
- Verify baud rate (default 115200)
- Check USB cable quality
- Try different USB port

### Component Not Found

**Problem**: `CMake Error: Component "xxx" not found`

**Solution**:
1. Verify component directory structure
2. Check CMakeLists.txt naming
3. Component name must match directory name

### Program Stuck/Crash

**Problem**: Device reboots continuously

**Solution**:
- Check stack overflow with `idf.py size`
- Enable watchdog timeout logs
- Check for NULL pointer dereferences

## Next Steps

1. **Read Architecture Documentation**: See `docs/ARCHITECTURE.md`
2. **Review Coding Standards**: See `docs/CODING_STANDARDS.md`
3. **Explore Components**: Check `components/` for available utilities
4. **Start Development**: Modify `main/app_init.c` and `main/app_tasks.c`

## Resources

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/)
- [FreeRTOS Documentation](https://www.freertos.org/index.html)
- [ESP32 Datasheet](https://www.espressif.com/en/products/socs/esp32)
- [Community Forum](https://esp32.com/)

## Support

For issues specific to this template, check the project documentation.

For ESP-IDF specific issues, refer to the official documentation or community forums.
