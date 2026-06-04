# C++ 项目快速开始指南

## 项目已就绪 ✅

您的ESP-IDF项目已成功转换为现代C++17项目。以下是快速开始步骤。

## 快速命令

### 1. 构建项目
```bash
cd c:\kk_data\code\esp32_idf\template_project
idf.py build
```

### 2. 刷写固件
```bash
idf.py flash
```

### 3. 监视输出
```bash
idf.py monitor
```

### 4. 完整工作流
```bash
idf.py build flash monitor
```

## 项目结构概览

```
main/
├── main.cpp                    # C++入口点
├── app_init.cpp/hpp           # 应用初始化类
├── app_tasks.cpp/hpp          # 应用任务类
├── app_manager.cpp/hpp        # 模块管理器（新增）
├── example_module.cpp/hpp     # 示例模块（新增）
└── include/
    ├── app_module.hpp         # 模块基类（新增）
    ├── app_manager.hpp        # 管理器类（新增）
    └── ...其他头文件
```

## 核心特性

### ✨ 现代C++17
- 异常处理启用
- 智能指针管理
- 命名空间组织
- RAII模式

### 🏗️ 模块系统
- 统一的模块接口
- 集中式管理器
- 生命周期管理
- 易于扩展

### 📦 编译优化
- C++17标准
- RTTI禁用（节省内存）
- 编译警告启用
- 性能优化

## 创建第一个自定义模块

### 步骤1：创建头文件
```cpp
// main/include/my_module.hpp
#include "app_module.hpp"

namespace app {
    class MyModule : public AppModule {
    public:
        bool initialize(void) override;
        bool deinitialize(void) override;
        bool is_initialized(void) const override;
        const std::string& get_name(void) const override;
        void process(void) override;
    };
}
```

### 步骤2：实现模块
```cpp
// main/my_module.cpp
#include "my_module.hpp"
#include "logger.h"

namespace app {
    bool MyModule::initialize(void) {
        LOG_INFO("MY_MODULE", "Initializing");
        return true;
    }
    // ... 实现其他方法
}
```

### 步骤3：注册模块
```cpp
// 在 main.cpp 的 app_main() 中
auto my_module = std::make_unique<app::MyModule>();
AppManager::get_instance().register_module(std::move(my_module));
```

### 步骤4：更新CMakeLists.txt
```cmake
idf_component_register(
    SRCS 
        "main.cpp"
        "app_init.cpp"
        "app_tasks.cpp"
        "app_manager.cpp"
        "my_module.cpp"    # 添加你的模块
    INCLUDE_DIRS 
        "."
        "include"
    # ...
)
```

## 关键代码示例

### 使用智能指针
```cpp
#include <memory>

// 创建对象
auto module = std::make_unique<app::MyModule>();

// 传递所有权
manager.register_module(std::move(module));
// 现在 module 为 nullptr，所有权已转移
```

### 异常处理
```cpp
try {
    if (!module->initialize()) {
        throw std::runtime_error("Initialize failed");
    }
} catch (const std::exception& e) {
    LOG_ERROR(TAG, "Error: %s", e.what());
}
```

### 命名空间使用
```cpp
using namespace app;  // 简化写法

auto manager = AppManager::get_instance();
auto tasks = std::make_unique<AppTasks>();
```

## 常见任务

### 访问日志系统
```cpp
#include "logger.h"

LOG_INFO(TAG, "信息: %d", value);
LOG_ERROR(TAG, "错误: %s", message);
LOG_WARN(TAG, "警告");
```

### 创建FreeRTOS任务
```cpp
xTaskCreate(
    task_function,
    "task_name",
    4096,           // 栈大小
    nullptr,        // 参数
    5,              // 优先级
    nullptr         // 任务句柄
);
```

### 使用配置宏
```cpp
#include "project_config.h"

printf("Project: %s\n", PROJECT_NAME);        // 从CMake定义
printf("Version: %s\n", PROJECT_VERSION);
printf("Build: %s\n", BUILD_TIMESTAMP);
```

## 文档索引

| 文档 | 描述 |
|------|------|
| [CPP_PROJECT_GUIDE.md](docs/CPP_PROJECT_GUIDE.md) | 完整的C++开发指南 |
| [CPP_MIGRATION_GUIDE.md](docs/CPP_MIGRATION_GUIDE.md) | C到C++迁移详细步骤 |
| [CPP_OPTIMIZATION_SUMMARY.md](docs/CPP_OPTIMIZATION_SUMMARY.md) | 项目优化总结 |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 项目架构设计 |
| [CODING_STANDARDS.md](docs/CODING_STANDARDS.md) | 编码规范 |
| [GETTING_STARTED.md](docs/GETTING_STARTED.md) | 原始入门指南 |

## 下一步

1. **阅读** [CPP_PROJECT_GUIDE.md](docs/CPP_PROJECT_GUIDE.md) 了解完整功能
2. **创建** 第一个自定义模块
3. **实现** 你的应用逻辑
4. **优化** 性能和内存使用

## 调试技巧

### 检查内存
```cpp
size_t free_heap = esp_get_free_heap_size();
LOG_INFO(TAG, "Free heap: %u bytes", free_heap);
```

### 检查栈
```cpp
UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
LOG_INFO(TAG, "Stack remaining: %u bytes", stack_left * 4);
```

### 启用GDB调试
```bash
idf.py gdb
```

## 故障排除

### 编译错误："undefined reference"
- 确保`.cpp`文件在CMakeLists.txt中
- 检查命名空间中的符号
- 验证头文件路径

### 运行时崩溃
- 检查堆栈大小是否足够
- 验证初始化顺序
- 使用日志追踪执行流程

### 内存不足
- 减少任务栈大小
- 启用`-Os`编译优化
- 检查内存泄漏

## 获取帮助

- ESP-IDF官方文档: https://docs.espressif.com/projects/esp-idf/
- C++参考: https://en.cppreference.com/
- 项目内文档: 查看`docs/`目录

---

**祝你编程愉快！** 🚀

如有问题，请参阅完整文档或项目内的示例代码。
