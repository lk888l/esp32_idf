# ESP-IDF C++ 项目指南

## 项目概述

此项目已优化为使用C++17作为主要开发语言。使用现代C++特性来改进代码质量、可维护性和安全性。

## 项目结构

```
main/
├── CMakeLists.txt              # 组件构建配置（支持C++）
├── main.cpp                    # 应用程序入口点（C++）
├── app_init.cpp               # 应用程序初始化实现
├── app_tasks.cpp              # 应用程序任务实现
├── app_manager.cpp            # 应用程序管理器实现
└── include/
    ├── app_init.hpp           # 初始化类头文件
    ├── app_tasks.hpp          # 任务类头文件
    ├── app_manager.hpp        # 管理器类头文件
    ├── app_module.hpp         # 模块基类头文件
    ├── app.h                  # 公共C兼容接口
    └── project_config.h       # 项目配置宏
```

## C++特性

### 编译设置
- **C++标准**: C++17 (`-std=c++17`)
- **异常处理**: 已启用 (`-fexceptions`)
- **RTTI**: 禁用 (`-fno-rtti`) - 用于减少存储空间

### 核心特性

#### 1. 命名空间组织
所有应用代码在`app`命名空间中组织：

```cpp
namespace app {
    class AppInit { /* ... */ };
    class AppTasks { /* ... */ };
    class AppManager { /* ... */ };
}
```

#### 2. RAII模式
资源在构造函数中获取，在析构函数中释放：

```cpp
class AppInit {
public:
    AppInit() { /* 初始化资源 */ }
    ~AppInit() { /* 释放资源 */ }
    
private:
    AppInit(const AppInit&) = delete;  // 禁用复制
    AppInit& operator=(const AppInit&) = delete;
};
```

#### 3. 现代内存管理
使用智能指针替代原始指针：

```cpp
auto task_processor = std::make_unique<app::AppTasks>();
auto module = std::make_unique<CustomModule>();
manager.register_module(std::move(module));
```

#### 4. 异常处理
系统错误由异常处理，防止错误传播：

```cpp
try {
    app::AppInit app_init_manager;
    if (!app_init_manager.initialize()) {
        throw std::runtime_error("Initialization failed");
    }
} catch (const std::exception& e) {
    LOG_ERROR(TAG, "Exception: %s", e.what());
}
```

## 开发指南

### 创建新的应用模块

继承`AppModule`基类：

```cpp
// module.hpp
#include "app_module.hpp"

namespace app {

class MyModule : public AppModule {
public:
    MyModule();
    ~MyModule() override;
    
    bool initialize(void) override;
    bool deinitialize(void) override;
    bool is_initialized(void) const override;
    const std::string& get_name(void) const override;
    void process(void) override;

private:
    bool initialized_;
    std::string name_ = "MyModule";
};

}  // namespace app
```

```cpp
// module.cpp
#include "module.hpp"
#include "logger.h"

namespace app {

MyModule::MyModule() : initialized_(false) {}

bool MyModule::initialize(void) {
    // Your initialization code here
    initialized_ = true;
    return true;
}

// ... 其他实现 ...

}  // namespace app
```

### 在应用程序中注册模块

```cpp
// 在 main.cpp 的 app_main() 中
auto my_module = std::make_unique<app::MyModule>();
AppManager::get_instance().register_module(std::move(my_module));

AppManager::get_instance().initialize_all();
AppManager::get_instance().process_all();  // 在主循环中调用
```

## 编译配置

### CMakeLists.txt 设置

`main/CMakeLists.txt`已配置为：

- 编译所有`.cpp`源文件
- 启用C++17标准
- 启用异常处理
- 禁用RTTI以节省空间
- 添加严格的编译警告

### 构建项目

```bash
idf.py build
idf.py flash
idf.py monitor
```

## C/C++ 互操作

### 从C代码调用C++

使用`extern "C"`包装器：

```cpp
// app.hpp
extern "C" {
    esp_err_t app_init_wrapper(void);
    esp_err_t app_deinit_wrapper(void);
}

// app.cpp
extern "C" esp_err_t app_init_wrapper(void) {
    app::AppInit init;
    return init.initialize() ? ESP_OK : ESP_FAIL;
}
```

### 从C++代码调用C

直接包含C头文件（已自动处理`extern "C"`）：

```cpp
#include "logger.h"  // C库
#include "esp_log.h" // ESP-IDF C库

LOG_INFO(TAG, "C++ 应用程序");
```

## 最佳实践

### 1. 内存管理
- ✅ 使用`std::unique_ptr`和`std::shared_ptr`
- ✅ 避免`new`和`delete`
- ❌ 避免原始指针所有权转移
- ❌ 避免内存泄漏

### 2. 异常处理
- ✅ 在关键初始化代码中使用异常
- ✅ 在构造函数中验证资源
- ❌ 不要忽视异常
- ❌ 不要在ISR中使用异常

### 3. 编码风格
- ✅ 使用命名空间避免命名冲突
- ✅ 使用const正确性
- ✅ 使用移动语义
- ❌ 不要使用全局可变状态
- ❌ 不要过度使用虚函数

### 4. 调试
- 使用`LOG_INFO`、`LOG_ERROR`等宏记录日志
- 编译时设置`-g`标志以包含调试符号
- 使用GDB进行调试

## 故障排除

### 编译错误

如果遇到链接错误，确保：
1. 所有`.cpp`文件都在`CMakeLists.txt`的`SRCS`中列出
2. 所有头文件都有适当的`#include`
3. 所有符号都在正确的命名空间中

### 运行时错误

如果遇到运行时错误：
1. 检查日志输出以获取错误信息
2. 确保所有模块初始化成功
3. 验证内存使用情况（使用`free` / `DRAM`命令）

## 常见问题

**Q: 为什么禁用RTTI?**
A: RTTI增加代码大小和开销。由于ESP32的内存限制，禁用它可以节省空间。需要RTTI时，通过编辑CMakeLists.txt重新启用。

**Q: 我可以混合使用C和C++代码吗?**
A: 是的。所有C库都自动与C++兼容。在C++中包含C头文件时，编译器会自动处理`extern "C"`。

**Q: 如何在ISR中使用C++?**
A: ISR应保持简单，不应使用异常或复杂的C++特性。使用信号量或消息队列与任务通信。

## 参考资源

- [ESP-IDF编程指南](https://docs.espressif.com/projects/esp-idf/)
- [C++17标准](https://en.cppreference.com/)
- [现代C++最佳实践](https://github.com/isocpp/CppCoreGuidelines)

## 许可证

请参阅项目根目录中的LICENSE文件。
