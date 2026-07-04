# esp_idf_template

这是一个面向 ESP-IDF 的 C++ 应用模板，提供基础的模块生命周期管理、FreeRTOS 任务封装和轻量日志接口。当前示例在 `main` 中注册一个 GPIO 闪烁任务，用于验证模板启动流程。

## 目录结构

```text
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.cpp
└── components/
    ├── app/
    ├── base/
    └── logger/
```

## 模板组件

- `components/app`
  - `AppModule`: 应用模块接口，定义初始化、反初始化和轮询入口。
  - `AppManager`: 管理模块注册、初始化顺序和主循环轮询。
  - `AppTask`: C++ 风格的 FreeRTOS task 封装，支持启动、停止和协作退出。
- `components/base`
  - `BasicObject`: 基于 FreeRTOS task notification 的基础对象。
  - `TaskReactor`: notification bit 分发工具。
  - `atomic_array`: 简单环形缓冲工具。
- `components/logger`
  - `Logger`: 轻量日志封装。

## GPIO 闪烁示例

`main/main.cpp` 中包含一个最小示例：

- `GpioBlinkModule` 继承 `AppModule`，负责 GPIO 初始化和任务生命周期。
- `GpioBlinkTask` 继承 `AppTask`，在独立 FreeRTOS 任务中周期性翻转 GPIO。
- `app_main()` 注册模块后调用 `AppManager::initialize_all()` 启动应用。

默认配置在 `main/main.cpp` 顶部：

```cpp
constexpr gpio_num_t kBlinkGpio = GPIO_NUM_8;
constexpr uint32_t kBlinkIntervalMs = 500;
```

修改闪烁引脚或周期时，直接调整这两个常量。

## 构建

目标芯片默认是 `esp32c5`，对应配置在 `sdkconfig.defaults` 中。

在 ESP-IDF 环境中执行：

```bash
idf.py set-target esp32c5
idf.py build
```

刷写并打开串口监视器：

```bash
idf.py -p <PORT> flash monitor
```

如果 ESP-IDF 在 Docker 容器 `esp--dev` 中，进入容器后切到项目目录执行同样命令：

```bash
docker exec -it esp--dev bash
cd <project-path-in-container>
idf.py build
idf.py -p <PORT> flash monitor
```

## 添加模块

小型功能可以直接在 `main` 中实现一个 `AppModule`，并在 `app_main()` 中注册：

```cpp
auto& manager = app::AppManager::get_instance();
manager.register_module(std::make_unique<MyModule>());
manager.initialize_all();
```

模块需要后台任务时，可以组合或继承 `AppTask`。任务主循环应定期检查 `shouldExit()`，以便 `stop()` 能正常退出。

当功能需要复用、依赖较多或源码规模变大时，再拆成独立 ESP-IDF component。
