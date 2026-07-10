# gpio_button_project

基于 `esp_idf_template` 的 C++ GPIO 按键工程，支持单击和长按。当前示例使用 `GPIO0`：按键一端接 GPIO，另一端接 GND，使用内部上拉（低电平按下）。请按实际开发板修改 `main/main.cpp` 中的 `kButtonGpio`，并避开启动绑带和已被外设占用的引脚。

完整的模块设计、API、状态机、接线、参数调优和故障排查说明见 [`doc/gpio-button-module.md`](doc/gpio-button-module.md)。

## 架构

```text
GPIO 边沿中断 -> 共享 ButtonService 工作任务 -> 固定长度事件队列 -> AppModule::process()
```

- 一个按键仅注册一个极短 ISR：记录边沿时间、置任务通知位；不读取 GPIO、不做日志、不分配内存。
- 所有按键共享一个工作任务（最多 16 个），只在边沿、去抖到期或长按到期时运行；空闲时无限期阻塞，不轮询。
- 去抖后才读取 GPIO。释放前未上报长按则产生 `Click`；保持按下达到阈值立即产生一次 `LongPress`。
- 事件队列默认 16 项，满时不阻塞 ISR/工作任务，而是丢弃最新事件并可用 `dropped_event_count()` 读取计数。

## 配置与事件

在 `main/main.cpp` 中示例配置：

```cpp
const button::ButtonConfig config{
    .gpio = GPIO_NUM_0,
    .active_low = true,
    .pull_mode = GPIO_PULLUP_ONLY,
    .debounce_ms = 20,
    .long_press_ms = 800,
};
```

调用 `add_button()` 后再调用 `start()`；运行后通过 `try_pop()` 或 `wait_pop()` 取得 `ButtonEvent`。为保证低延迟和吞吐，事件消费者不应在 `ButtonService` 的工作任务上下文中做耗时操作；本工程把日志消费放在 `AppModule::process()`。

## 构建

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

`sdkconfig.defaults` 将 FreeRTOS tick 配置为 1 kHz，因此 20 ms 去抖和 800 ms 长按按毫秒精度工作。
