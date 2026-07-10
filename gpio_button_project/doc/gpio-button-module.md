# GPIO Button 模块设计与使用说明

## 1. 文档范围

本文说明 `components/button` 中 `button::GpioButtonService` 的设计、接口、运行时行为和接入方式。

该模块基于 ESP-IDF、FreeRTOS 和 C++20，实现以下功能：

- 单击事件 `ButtonEventType::Click`；
- 长按事件 `ButtonEventType::LongPress`；
- 最多同时管理 16 个 GPIO 按键；
- 每个 GPIO 使用边沿中断，所有按键共享一个工作任务；
- 使用固定容量的 FreeRTOS 事件队列解耦按键检测和业务处理；
- 支持高电平或低电平有效，以及内部上拉、内部下拉或外部上下拉电阻。

模块不包含双击、连击、按下、释放和长按连发事件。这些行为可在现有事件模型上继续扩展。

## 2. 文件与依赖

```text
gpio_button_project/
├── components/
│   └── button/
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── gpio_button_service.hpp
│       └── src/
│           └── gpio_button_service.cpp
├── doc/
│   └── gpio-button-module.md
└── main/
    └── main.cpp
```

组件依赖：

```cmake
idf_component_register(
    SRCS
        "src/gpio_button_service.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        freertos
)
```

- `driver`：GPIO 配置、中断服务和 GPIO 电平读取；
- `freertos`：任务通知、工作任务、事件队列和系统 tick。

使用该模块的 ESP-IDF component 需要在自己的 `CMakeLists.txt` 中声明 `button`：

```cmake
idf_component_register(
    SRCS "main.cpp"
    REQUIRES button
)
```

## 3. 设计目标

### 3.1 低 CPU 占用

模块不周期性轮询 GPIO。没有按键边沿、去抖截止时间或长按截止时间时，工作任务通过 `xTaskNotifyWait()` 使用 `portMAX_DELAY` 无限期阻塞。

### 3.2 低中断开销

GPIO ISR 只执行以下操作：

1. 读取当前 FreeRTOS tick；
2. 更新该按键的最近边沿时间和边沿代数；
3. 通过 task notification 唤醒共享工作任务；
4. 必要时触发一次 ISR 退出后的任务切换。

ISR 不读取 GPIO、不执行去抖、不写日志、不创建对象，也不向事件队列直接发送事件。ISR 使用 `IRAM_ATTR`，GPIO ISR service 使用 `ESP_INTR_FLAG_IRAM` 安装。

### 3.3 多按键共享资源

每个按键拥有一个 `ButtonSlot` 和一个 GPIO ISR handler，但最多 16 个按键共享：

- 一个 FreeRTOS 工作任务；
- 一个事件队列；
- 一套去抖和长按调度逻辑。

因此增加按键不会为每个按键额外创建任务和任务栈。

### 3.4 业务解耦

检测任务只生成 `ButtonEvent`。业务代码通过 `try_pop()` 或 `wait_pop()` 消费事件，慢速日志、状态切换或应用逻辑不会在 GPIO ISR 中运行。

## 4. 总体数据流

```text
        GPIO 任意边沿
              │
              ▼
      gpio_isr()，IRAM ISR
      - 记录最近边沿 tick
      - edge_generation++
      - task notification
              │
              ▼
     共享 gpio_buttons 任务
      - 等待去抖窗口结束
      - 读取稳定 GPIO 电平
      - 更新按下/释放状态
      - 检查长按截止时间
              │
              ▼
       FreeRTOS 事件队列
       - Click
       - LongPress
              │
              ▼
     应用任务或 AppModule
```

任务通知可以合并多个中断唤醒，但 `edge_generation` 会记录新的边沿是否尚未处理。去抖只关心最新边沿后电平是否稳定，不需要保存机械抖动期间每一个边沿。

## 5. 公共数据类型

### 5.1 `ButtonEventType`

```cpp
enum class ButtonEventType : uint8_t {
    Click,
    LongPress,
};
```

| 枚举值 | 产生时机 |
|---|---|
| `Click` | 按下和释放都已稳定确认，并且本次按下尚未产生长按事件 |
| `LongPress` | 稳定按下持续时间达到 `long_press_ms`，每次按下最多产生一次 |

长按后释放不会再产生 `Click`。

### 5.2 `ButtonEvent`

```cpp
struct ButtonEvent {
    gpio_num_t gpio;
    ButtonEventType type;
    uint32_t timestamp_ms;
};
```

| 字段 | 含义 |
|---|---|
| `gpio` | 产生事件的 GPIO 编号，可用于区分多个按键 |
| `type` | 单击或长按 |
| `timestamp_ms` | 事件发布时的系统 tick 换算值，单位为毫秒 |

`Click` 的时间戳位于释放去抖确认完成时；`LongPress` 的时间戳位于长按阈值到达并确认按键仍然按下时。因此它不是原始 GPIO 边沿的精确时间戳。

`timestamp_ms` 来源为 `xTaskGetTickCount() * portTICK_PERIOD_MS`。当前工程使用 1 kHz FreeRTOS tick。若 `TickType_t` 为 32 位，时间戳会随 tick 在约 49.7 天后回绕；应用应使用无符号差值处理持续时间，不应假设该值永久单调递增。

### 5.3 `ButtonConfig`

```cpp
struct ButtonConfig {
    gpio_num_t gpio;
    bool active_low = true;
    gpio_pull_mode_t pull_mode = GPIO_PULLUP_ONLY;
    uint16_t debounce_ms = 20;
    uint16_t long_press_ms = 800;
};
```

| 字段 | 默认值 | 说明 |
|---|---:|---|
| `gpio` | 无 | GPIO 编号，必须处于当前芯片的有效 GPIO 范围内 |
| `active_low` | `true` | `true` 表示低电平为按下，`false` 表示高电平为按下 |
| `pull_mode` | `GPIO_PULLUP_ONLY` | GPIO 内部上下拉模式 |
| `debounce_ms` | `20` | 最后一个边沿后必须保持稳定的时间 |
| `long_press_ms` | `800` | 从稳定按下确认开始计算的长按阈值 |

配置约束：

- `debounce_ms` 不能为 0；
- `long_press_ms` 必须大于 `debounce_ms`；
- 同一个 service 中不能重复注册相同 GPIO；
- 必须在 `start()` 之前调用 `add_button()`。

## 6. `GpioButtonService` API

### 6.1 构造函数

```cpp
explicit GpioButtonService(
    std::size_t event_queue_length = 16,
    uint32_t task_stack_size = 2560,
    UBaseType_t task_priority = tskIDLE_PRIORITY + 2);
```

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `event_queue_length` | `16` | 可缓存的 `ButtonEvent` 数量 |
| `task_stack_size` | `2560` | ESP-IDF `xTaskCreate()` 使用的任务栈大小，单位为字节 |
| `task_priority` | `tskIDLE_PRIORITY + 2` | 共享检测任务优先级 |

对象内部的按键表为 `std::array<ButtonSlot, 16>`，在对象创建时固定存在。事件队列和工作任务在 `start()` 时创建。

析构函数会调用 `stop()`。由于 `stop()` 可能等待工作任务退出，不应从 ISR 或该 service 自己的工作任务中析构对象。

### 6.2 `add_button()`

```cpp
bool add_button(const ButtonConfig& config);
```

成功时保存配置并返回 `true`。以下情况返回 `false`：

- service 已经启动；
- 已注册 16 个按键；
- GPIO 编号无效；
- `debounce_ms == 0`；
- `long_press_ms <= debounce_ms`；
- GPIO 已经注册。

### 6.3 `start()`

```cpp
bool start();
```

启动过程依次执行：

1. 创建事件队列；
2. 配置所有 GPIO 为输入、上下拉模式和任意边沿中断；
3. 读取每个按键的启动初始状态；
4. 安装进程级 GPIO ISR service；
5. 创建共享 `gpio_buttons` 工作任务；
6. 为每个 GPIO 注册 ISR handler。

以下情况会返回 `false`：

- service 已启动；
- 没有注册按键；
- 事件队列长度为 0；
- 队列内存分配失败；
- GPIO 配置失败；
- GPIO ISR service 安装失败；
- 工作任务创建失败；
- 任一 GPIO ISR handler 注册失败。

如果其他组件已经安装了 GPIO ISR service，ESP-IDF 返回 `ESP_ERR_INVALID_STATE`。模块将其视为可共享状态并继续运行。

### 6.4 `stop()`

```cpp
bool stop(TickType_t timeout = pdMS_TO_TICKS(1000));
```

停止过程会移除各 GPIO handler、请求工作任务退出、唤醒可能正在阻塞的任务，并在任务退出后删除事件队列。超时前成功退出返回 `true`，否则返回 `false`。

模块不会调用 `gpio_uninstall_isr_service()`，因为 GPIO ISR service 属于整个进程，其他组件可能仍在使用它。

停止后，已注册的 `ButtonConfig` 仍保留，可以再次调用 `start()`。此时也可以在重新启动前继续添加未重复的 GPIO，直到达到 16 个上限。

### 6.5 `try_pop()`

```cpp
bool try_pop(ButtonEvent& event);
```

非阻塞读取事件。队列中有事件时复制到 `event` 并返回 `true`；队列为空或 service 未创建队列时返回 `false`。

适合在已有周期性应用循环或 `AppModule::process()` 中使用。

### 6.6 `wait_pop()`

```cpp
bool wait_pop(ButtonEvent& event, TickType_t timeout);
```

等待事件，最长等待 `timeout`。可传入 `portMAX_DELAY` 创建完全事件驱动的消费者任务。

如果多个任务同时消费同一个队列，每个事件只会被其中一个任务收到；该队列不是广播机制。

### 6.7 状态与诊断接口

```cpp
uint32_t dropped_event_count() const;
bool is_running() const;
std::size_t button_count() const;
```

- `dropped_event_count()`：事件队列已满时丢弃的新事件总数；停止、重启不会自动清零；
- `is_running()`：工作任务是否处于运行状态；
- `button_count()`：已经注册的按键数量。

## 7. 去抖与事件状态机

### 7.1 边沿去抖

GPIO 配置为 `GPIO_INTR_ANYEDGE`。每次边沿都会更新 `last_edge_tick` 和 `edge_generation`。

工作任务醒来后不会立即把当前电平当成有效状态，而是等待：

```text
当前 tick - 最后边沿 tick >= debounce_ms
```

若等待期间又发生边沿，`last_edge_tick` 被更新，去抖窗口从最新边沿重新计算。窗口到期后只读取一次 GPIO，并将逻辑电平与上一次稳定状态比较。

这种算法不会统计抖动次数，只确认最后一次边沿后的稳定状态，适合机械按键。

### 7.2 单击

```text
稳定未按下
    │ 按下边沿 + 去抖确认
    ▼
稳定按下，记录 press_tick
    │ 在 long_press_ms 前释放 + 去抖确认
    ▼
发布 Click
    ▼
稳定未按下
```

单击事件在释放确认后产生，因此单击延迟至少包含释放端的 `debounce_ms`。

### 7.3 长按

```text
稳定未按下
    │ 按下边沿 + 去抖确认
    ▼
稳定按下，记录 press_tick
    │ 持续达到 long_press_ms
    ▼
发布一次 LongPress
    │ 后续继续按住不重复发布
    │ 释放 + 去抖确认
    ▼
稳定未按下，不再发布 Click
```

长按计时从“按下已经通过去抖确认”的时刻开始，不从第一次原始 GPIO 边沿开始。因此从物理按下到长按事件的典型总时间约为：

```text
debounce_ms + long_press_ms
```

### 7.4 阈值附近释放

如果释放边沿发生在长按阈值附近，但释放仍处于去抖窗口，工作任务会检查当前 GPIO 电平：

- 当前已经是释放电平：暂不发布长按，等待释放去抖完成；
- 当前仍是按下电平：允许发布长按。

这样可以避免一个在长按阈值前刚刚释放的短按被错误转换为长按。

### 7.5 启动时已经按下

`start()` 配置 GPIO 后会读取一次初始状态。如果启动时按键已经处于有效按下电平，该状态会成为初始稳定状态，并从启动时开始计算长按；保持按下达到阈值后会产生 `LongPress`。

如果设备一启动就持续产生长按，首先检查按键极性、上下拉配置和实际静态电平。

## 8. 工作任务调度

每次循环调用 `process_buttons(now)`，它会检查所有已注册按键，并计算下一个最近截止时间：

- 某按键剩余的去抖时间；
- 某按键剩余的长按时间；
- 若没有任何截止时间，则为 `portMAX_DELAY`。

然后工作任务调用：

```cpp
xTaskNotifyWait(0, UINT32_MAX, &notification_bits, next_wait);
```

因此任务会在以下任一条件满足时运行：

- 任意按键发生 GPIO 边沿并发送 notification；
- 最近的去抖截止时间到达；
- 最近的长按截止时间到达；
- `stop()` 请求任务退出。

一次唤醒会扫描最多 16 个固定槽位。该扫描复杂度为 O(N)，但 N 上限很小，并且只在事件或计时器到期时执行。

## 9. 事件队列和背压

检测任务使用 `xQueueSend(..., 0)` 非阻塞发布事件。

如果事件队列已满：

- 最新事件被丢弃；
- ISR 和检测任务都不会等待消费者；
- `dropped_event_count()` 增加 1。

这种策略优先保护按键检测时序和系统实时性。若不能接受事件丢失，应：

1. 增大构造函数的 `event_queue_length`；
2. 提高消费者执行频率；
3. 减少消费者中的日志、Flash 写入或其他阻塞操作；
4. 周期性监控 `dropped_event_count()`。

## 10. 接线与极性

### 10.1 低电平按下，内部上拉

推荐的简单接法：

```text
GPIO ---- 按键 ---- GND
  │
  └── ESP32 内部上拉
```

```cpp
ButtonConfig{
    .gpio = GPIO_NUM_0,
    .active_low = true,
    .pull_mode = GPIO_PULLUP_ONLY,
    .debounce_ms = 20,
    .long_press_ms = 800,
};
```

### 10.2 高电平按下，内部下拉

```text
3.3 V ---- 按键 ---- GPIO
                     │
                     └── ESP32 内部下拉
```

```cpp
ButtonConfig{
    .gpio = GPIO_NUM_5,
    .active_low = false,
    .pull_mode = GPIO_PULLDOWN_ONLY,
    .debounce_ms = 20,
    .long_press_ms = 800,
};
```

不要向 ESP32 GPIO 输入超过芯片允许范围的电压。选脚时还需要避开 Flash、启动绑带、USB/JTAG 或其他板载外设正在使用的 GPIO。当前示例使用 `GPIO0`，必须结合具体开发板原理图确认其启动和复用影响。

使用外部上下拉电阻时，可以根据硬件设计选择 `GPIO_FLOATING`，同时确保 GPIO 在按键断开时不会悬空。

## 11. 单按键使用示例

```cpp
#include "gpio_button_service.hpp"

button::GpioButtonService buttons;

bool initialize_buttons()
{
    const button::ButtonConfig config{
        .gpio = GPIO_NUM_0,
        .active_low = true,
        .pull_mode = GPIO_PULLUP_ONLY,
        .debounce_ms = 20,
        .long_press_ms = 800,
    };

    return buttons.add_button(config) && buttons.start();
}

void process_buttons()
{
    button::ButtonEvent event{};
    while (buttons.try_pop(event)) {
        switch (event.type) {
        case button::ButtonEventType::Click:
            // 处理单击
            break;
        case button::ButtonEventType::LongPress:
            // 处理长按
            break;
        }
    }
}
```

当前工程的 `ButtonModule::process()` 每 10 ms 调用一次，并通过 `while (try_pop())` 一次排空当前所有事件。

## 12. 多按键使用示例

```cpp
button::GpioButtonService buttons(32, 2560, tskIDLE_PRIORITY + 2);

bool initialize_buttons()
{
    const button::ButtonConfig up{
        .gpio = GPIO_NUM_4,
        .active_low = true,
        .pull_mode = GPIO_PULLUP_ONLY,
        .debounce_ms = 20,
        .long_press_ms = 700,
    };

    const button::ButtonConfig down{
        .gpio = GPIO_NUM_5,
        .active_low = true,
        .pull_mode = GPIO_PULLUP_ONLY,
        .debounce_ms = 20,
        .long_press_ms = 700,
    };

    return buttons.add_button(up) &&
           buttons.add_button(down) &&
           buttons.start();
}

void dispatch(const button::ButtonEvent& event)
{
    if (event.gpio == GPIO_NUM_4) {
        // UP 按键
    } else if (event.gpio == GPIO_NUM_5) {
        // DOWN 按键
    }
}
```

每个按键可以使用不同的极性、上下拉、去抖时间和长按阈值。

## 13. 阻塞式消费者示例

如果应用不使用周期性 `process()`，可以创建消费者任务：

```cpp
void button_event_task(void* arg)
{
    auto* buttons = static_cast<button::GpioButtonService*>(arg);
    button::ButtonEvent event{};

    while (true) {
        if (buttons->wait_pop(event, portMAX_DELAY)) {
            // 在普通任务上下文处理事件
        }
    }
}
```

service 对象的生命周期必须长于消费者任务。停止或析构 service 前，应先通知消费者任务退出，避免消费者继续访问已删除的队列。

## 14. 参数调优建议

### `debounce_ms`

- 常见机械按键：10～30 ms；
- 抖动明显、线缆较长：20～50 ms；
- 霍尔、光电或硬件整形信号：可适当缩短，但不能设为 0。

值越大，抗抖能力越强，但按下和释放确认延迟也越大。不要用很大的去抖时间掩盖接线悬空、电源噪声或错误的上下拉配置。

### `long_press_ms`

- 快速操作界面：500～800 ms；
- 防误触操作：1000～2000 ms；
- 危险操作应在应用层增加二次确认，而不只依赖更长阈值。

该值必须大于 `debounce_ms`。

### 任务优先级

默认 `tskIDLE_PRIORITY + 2` 通常足够。系统中存在长时间不让出 CPU 的高优先级任务时，按键处理延迟会增加。不要盲目把按键任务提升到高于关键控制、通信或安全任务的优先级。

### 任务栈

检测任务只执行 GPIO 读取、状态计算和队列发送，默认 2560 字节留有余量。扩展模块时不要在检测任务中添加日志格式化、大型局部数组或复杂回调；如果确需添加，应通过 ESP-IDF 栈水位接口重新测量。

## 15. 并发与调用上下文

| 接口 | 允许的上下文 | 说明 |
|---|---|---|
| `add_button()` | 普通任务，启动前 | 不支持运行时动态注册，不应并发调用 |
| `start()` | 普通任务 | 会创建队列、任务和 ISR handler |
| `stop()` | 普通任务 | 可能阻塞等待工作任务退出 |
| `try_pop()` | 普通任务 | 非阻塞；多个消费者会竞争事件 |
| `wait_pop()` | 普通任务 | 可阻塞；不能从 ISR 调用 |
| `dropped_event_count()` | 普通任务 | 原子读取 |

`GpioButtonService` 不是通用的全接口线程安全容器。推荐由一个应用模块管理其配置和生命周期，其他任务只通过事件队列消费结果。

## 16. 资源占用特征

固定资源：

- `GpioButtonService` 对象内包含 16 个 `ButtonSlot`；
- 最多注册 16 个 GPIO handler；
- 一个共享工作任务；
- 一个 FreeRTOS 事件队列。

可配置资源：

- 工作任务栈，默认 2560 字节；
- 事件队列长度，默认 16 个事件。

CPU 使用特征：

- 空闲时不轮询，工作任务阻塞；
- 每个 GPIO 边沿执行一次极短 ISR；
- 去抖或长按到期时扫描已注册按键；
- 队列满时不阻塞实时检测路径。

## 17. 常见问题排查

### 完全没有事件

依次检查：

1. `add_button()` 和 `start()` 的返回值；
2. GPIO 是否为当前芯片有效输入引脚；
3. 按键是否真正连接到配置的 GPIO；
4. `active_low` 是否与接线一致；
5. `pull_mode` 是否能在按键断开时提供稳定电平；
6. GPIO 是否被其他组件重新配置；
7. 应用是否持续调用 `try_pop()`，或是否存在 `wait_pop()` 消费者。

### 上电后立即进入按下或长按状态

通常是极性或上下拉配置错误，也可能是按键在启动时已经按下。使用万用表或逻辑分析仪确认未按下和按下时的实际 GPIO 电平。

### 偶发误触发

- 适当增加 `debounce_ms`；
- 检查输入是否悬空；
- 缩短按键线缆并远离电机、继电器和高速时钟；
- 必要时增加外部上下拉、RC 滤波或施密特触发器；
- 检查地线和电源完整性。

### 快速操作时事件丢失

读取 `dropped_event_count()`：

- 计数增加：事件队列消费过慢或容量过小；
- 计数不增加：检查 `debounce_ms` 是否把过快边沿按机械抖动合并。

### `start()` 返回 `false`

重点检查：

- 是否没有添加按键；
- 是否重复启动；
- 是否存在重复或无效 GPIO；
- 是否设置了 `long_press_ms <= debounce_ms`；
- 系统是否有足够 heap 创建队列和任务；
- GPIO ISR service 或 handler 是否与其他组件冲突。

## 18. 当前限制与扩展方向

当前限制：

- 一个 service 最多管理 16 个按键；
- 启动后不能动态添加或移除按键；
- 只输出单击和单次长按；
- 队列事件是单消费者语义，不是广播；
- 时间基于 FreeRTOS tick，不是微秒级硬件时间戳；
- 模块直接依赖 ESP-IDF GPIO 和 FreeRTOS，不是跨平台纯 C++ 驱动。

可扩展方向：

- 增加 `Pressed`、`Released`、`DoubleClick`、`Repeat`；
- 为每个按键增加用户 ID，避免业务层直接依赖 GPIO 编号；
- 使用静态队列和静态任务进一步消除运行时 heap 分配；
- 增加运行时统计，例如最大队列深度和 ISR/任务处理计数；
- 把 GPIO 读取、时间源和事件输出抽象为接口，便于主机单元测试和跨平台移植。

## 19. 构建验证

工程目标芯片由 `sdkconfig.defaults` 配置为 ESP32-C5，FreeRTOS tick 为 1 kHz：

```text
CONFIG_IDF_TARGET="esp32c5"
CONFIG_FREERTOS_HZ=1000
```

标准 ESP-IDF 环境中执行：

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

当前模块已在本工程中通过 ESP-IDF 5.5.4 完整编译、链接和二进制生成验证。

