# 从C迁移到C++的指南

## 概述

本指南提供从C代码迁移到C++的步骤说明。

## 已完成的转换

### 1. 文件扩展名
- `main.c` → `main.cpp`
- `app_init.c` → `app_init.cpp`
- `app_tasks.c` → `app_tasks.cpp`

### 2. 头文件转换
- `app_init.h` → `app_init.hpp`
- `app_tasks.h` → `app_tasks.hpp`
- 添加了新的头文件：`app_module.hpp`, `app_manager.hpp`

### 3. 编译配置
- 更新了CMake以支持C++17编译
- 启用异常处理
- 禁用RTTI以优化大小

## 代码转换示例

### 旧C代码
```c
// main.c
void app_main(void) {
    esp_err_t ret = app_init();
    if (ret != ESP_OK) {
        LOG_ERROR("Init failed");
        esp_restart();
    }
    // ...
}

// 静态函数
static void main_task(void *pvParameter) {
    while (1) {
        app_tasks_process();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
```

### 新C++代码
```cpp
// main.cpp
extern "C" void app_main(void) {
    try {
        app::AppInit app_init;
        if (!app_init.initialize()) {
            throw std::runtime_error("Initialization failed");
        }
        // ...
    } catch (const std::exception& e) {
        LOG_ERROR("Exception: %s", e.what());
        esp_restart();
    }
}

// C++类方法
void main_task(void *pvParameter) {
    auto tasks = std::make_unique<app::AppTasks>();
    while (1) {
        tasks->process();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
```

## 迁移步骤

### 步骤1：规划模块
- 识别应该成为类的逻辑单元
- 计划类层次结构
- 定义接口（继承AppModule）

### 步骤2：创建类
```cpp
class MyFeature : public app::AppModule {
public:
    bool initialize(void) override;
    bool deinitialize(void) override;
    bool is_initialized(void) const override;
    const std::string& get_name(void) const override;
    void process(void) override;

private:
    // 私有成员和方法
};
```

### 步骤3：实现方法
```cpp
bool MyFeature::initialize(void) {
    // 从C转换的初始化代码
    if (!setup_hardware()) {
        return false;
    }
    initialized_ = true;
    return true;
}
```

### 步骤4：注册到管理器
```cpp
auto feature = std::make_unique<MyFeature>();
AppManager::get_instance().register_module(std::move(feature));
```

### 步骤5：测试
- 编译项目
- 检查日志输出
- 验证功能正确性

## C++特性逐步采用

### 第一步：基本转换
1. 转换文件为`.cpp`
2. 添加命名空间
3. 转换为类结构

### 第二步：现代C++特性
1. 使用`std::string`代替`char*`
2. 使用智能指针代替原始指针
3. 使用容器（`std::vector`, `std::map`）

### 第三步：高级特性
1. 使用异常处理
2. 使用模板
3. 使用现代算法（`std::transform`, 等）

## 兼容性注意事项

### C库集成
- ESP-IDF C库已自动与C++兼容
- 无需修改现有C API
- 可在C++中直接调用C函数

### FreeRTOS集成
- FreeRTOS API保持不变
- 可在C++中使用队列、信号量等
- ISR应保持简单，不使用C++特性

## 性能考虑

### 优化
- 使用`-O2`或`-O3`编译优化
- 启用链接时优化（LTO）
- 使用`inline`关键字优化热路径

### 权衡
- 异常处理有少量开销（禁用RTTI时最小）
- 虚函数调用比静态调用稍慢
- 智能指针有很小的开销

## 调试C++代码

### 常见问题

**链接错误：undefined reference**
- 检查符号在正确的命名空间中
- 检查所有`.cpp`文件都在CMakeLists.txt中列出

**运行时崩溃**
- 使用GDB进行调试：`idf.py gdb`
- 启用core dumps进行事后分析
- 检查堆栈溢出：`assert(uxTaskGetStackHighWaterMark(NULL) > 0)`

**编译警告**
- 检查未使用的变量
- 检查类型不匹配
- 使用`-Werror`将警告转为错误

## 性能监控

### 内存使用
```cpp
// 在C++代码中检查可用内存
size_t free_heap = esp_get_free_heap_size();
LOG_INFO(TAG, "Free heap: %u bytes", free_heap);
```

### CPU使用率
```cpp
// 使用FreeRTOS任务统计
vTaskGetRunTimeStats(...)
```

## 下一步

1. 逐步迁移其他模块到C++
2. 实现更高级的设计模式
3. 添加单元测试框架
4. 优化性能关键路径

## 参考

- [ESP-IDF C++支持](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/cplusplus.html)
- [FreeRTOS在ESP32上的使用](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html)
- [现代C++最佳实践](https://github.com/isocpp/CppCoreGuidelines)
