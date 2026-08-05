# esp_idf_template

这是一个面向 ESP-IDF 5.5 的 C++20 应用模板，默认目标为 ESP32-C3。模板使用工厂函数构造应用模块，并由固定容量的 `AppManager` 统一管理模块注册、初始化、失败回滚、轮询和反初始化。

当前最小示例通过 `create_gpio_blink_module()` 创建 GPIO8 闪烁模块，任务每 500 ms 翻转一次输出电平。

## 目录结构

```text
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.cpp
├── components/
│   ├── app/
│   ├── app_modules/
│   ├── base/
│   └── logger/
└── tests/
    ├── CMakeLists.txt
    └── host_tests.cpp
```

## 应用架构

- `components/app`
  - `AppModule`：统一维护 `stopped`、`initialized` 和 `cleanup_failed` 状态；具体模块只实现 `on_initialize()` 与 `on_deinitialize()`。
  - `AppManager`：使用固定 8 槽注册表，按注册顺序初始化并按反序停止或回滚。
  - `AppTask`：C++ 风格的 FreeRTOS task 封装，支持协作退出和带超时的安全停止。
- `components/app_modules`
  - 对外只公开 `create_*_module()` 工厂函数。
  - 具体模块类和任务类隐藏在 `src` 中，避免 `main` 依赖实现细节。
- `components/base` 与 `components/logger`
  - 保留为可选工具组件；当前主示例使用 ESP-IDF 原生 `ESP_LOG`。
- `main`
  - 只作为组合入口：构造管理器、调用工厂、注册模块、启动生命周期并运行主循环。

`AppManager` 最多注册 8 个名称唯一的模块。初始化失败时，已启动模块会按反序清理；如果任何模块清理失败，管理器进入 faulted 状态并保留模块存储，避免仍在运行的任务访问已销毁对象。

## GPIO8 闪烁示例

`GpioBlinkModule` 和 `GpioBlinkTask` 位于 `components/app_modules/src/gpio_blink_module.cpp`，外部只使用：

```cpp
app::AppManager manager;
manager.register_module(app_modules::create_gpio_blink_module());
manager.initialize_all();
```

默认参数：

- 输出引脚：GPIO8
- 翻转周期：500 ms
- 完整亮灭周期：约 1 s
- task 栈：2048 bytes
- task 优先级：`tskIDLE_PRIORITY + 1`

如果开发板没有把 GPIO8 接到可见 LED，仍可通过串口启动日志验证模块和任务已经运行。

## 添加模块

1. 在 `components/app_modules/include/app_modules.hpp` 声明返回 `std::unique_ptr<AppModule>` 的工厂函数。
2. 在 `components/app_modules/src` 中定义具体模块，并实现稳定且唯一的 `name()`、`on_initialize()` 和 `on_deinitialize()`。
3. 后台任务继承 `AppTask`，循环中定期检查 `should_exit()`，确保 `stop()` 可以协作结束任务。
4. 在 `app_main()` 中按依赖顺序调用工厂并注册模块；管理器会按相反顺序清理。

模块名作为注册表键，返回的 `std::string_view` 必须引用生命周期长于模块对象的存储，通常使用字符串字面量。

## 主机单元测试

无需连接开发板即可验证注册、回滚和清理语义：

```bash
cmake -S tests -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

## 在 esp-dev 中构建与烧录

宿主机项目在容器中挂载为 `/workspace/esp32_idf/esp_idf_template`：

```bash
docker exec -it -w /workspace/esp32_idf/esp_idf_template esp-dev bash
source /opt/esp/idf/export.sh
idf.py -B build/idf set-target esp32c3
idf.py -B build/idf build
idf.py -B build/idf -p /dev/ttyACM1 flash monitor
```

烧录前建议先确认串口对应的芯片：

```bash
esptool.py --port /dev/ttyACM1 chip_id
```

退出串口监视器使用 `Ctrl+]`。
