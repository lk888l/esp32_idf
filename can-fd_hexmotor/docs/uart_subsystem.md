# UART 子系统文档

## 1. 系统概述

UART 子系统是 ESP32 固件的**核心通信通道**，同时承担**日志输出**和**串口命令接收**两项职责。整个子系统围绕 ESP-IDF 原生 DMA UART 驱动构建，以零额外 FreeRTOS 原语、零中间拷贝为设计原则。

| 特性 | 说明 |
|------|------|
| 硬件接口 | UART_NUM_0 (TX=GPIO11, RX=GPIO12) |
| 波特率 | 115200 bps |
| 数据格式 | 8N1 (8数据位, 无校验, 1停止位) |
| 流控制 | 无硬件流控 |
| TX 机制 | DMA 环形缓冲区 → 硬件, 非阻塞 |
| RX 机制 | IDF ISR → DMA 环形缓冲区 → 事件队列 |
| 工作模式 | 全双工 |
| 日志路径 | `Logger` → `std::format`/`vsnprintf` → UART DMA |
| 命令路径 | UART RX → 事件队列 → `UartRxTask` → 命令解析 |

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                          UART 子系统                             │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    main.cpp (app_main)                     │   │
│  │                                                          │   │
│  │  ┌─────────────────┐    ┌─────────────────────────────┐  │   │
│  │  │  Logger          │    │  UartRxTask                 │  │   │
│  │  │  (日志输出)       │    │  (命令接收与解析)            │  │   │
│  │  │                 │    │                             │  │   │
│  │  │  • info/warn/   │    │  • xQueueReceive(event_q)   │  │   │
│  │  │    error/debug  │    │  • drainLines()             │  │   │
│  │  │  • vprintfHook  │    │  • parseStrCMD/parseStrArg  │  │   │
│  │  └────────┬────────┘    └──────────────┬──────────────┘  │   │
│  │           │                            │                  │   │
│  │           │    IUartDriver 接口          │                  │   │
│  │           └────────────┬───────────────┘                  │   │
│  │                        │                                   │   │
│  │           ┌────────────▼──────────────────────────┐       │   │
│  │           │       Esp32UartDmaDriver               │       │   │
│  │           │  (IDF DMA + 原生事件队列)              │       │   │
│  │           └────────────┬──────────────────────────┘       │   │
│  └────────────────────────┼─────────────────────────────────┘   │
│                           │                                     │
│  ┌────────────────────────▼─────────────────────────────────┐   │
│  │                   ESP-IDF UART 驱动层                      │   │
│  │  ┌──────────┐  DMA  ┌──────────┐ UART_DATA ┌───────────┐ │   │
│  │  │ UART HW  │──────▶│ IDF ISR  │──────────▶│ event_q   │ │   │
│  │  └──────────┘       └──────────┘           └───────────┘ │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 类继承关系

```
BasicObject
    └── IUartDriver (抽象接口)
            └── Esp32UartDmaDriver (DMA 实现)

Logger (独立类，静态持有 IUartDriver* 引用)
```

- **`BasicObject`** — 提供 `emit()` / `emitFromISR()` 信号发射能力（FreeRTOS TaskNotify 机制），使 UART 驱动可与 `TaskReactor` 配合使用
- **`IUartDriver`** — 纯虚接口，定义 UART 操作的完整契约
- **`Esp32UartDmaDriver`** — 唯一的具体实现，基于 ESP-IDF DMA UART 驱动

---

## 4. IUartDriver 接口

文件: [uart_driver.hpp](../components/bsp/include/uart_driver.hpp)

### 4.1 生命周期方法

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `init()` | `bool` | 安装 UART 驱动、配置参数、设置引脚 |
| `start()` | `bool` | 启动驱动，置运行标志 |
| `stop()` | `bool` | 停止驱动，删除 UART 驱动实例 |

### 4.2 TX 发送接口

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `write(data, len)` | `size_t` | 非阻塞写入，返回实际入队字节数 |
| `flush()` | `void` | 阻塞等待所有 TX 数据发送完毕（超时 1s） |
| `tx_pending()` | `size_t` | 查询 TX 环形缓冲区中待发送字节数 |

### 4.3 RX 接收接口 (轮询)

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `available()` | `size_t` | RX 环形缓冲区中可读取字节数 |
| `read(buf, max_len, timeout_ms)` | `size_t` | 读取数据，支持超时阻塞模式 |
| `readAvailable(buf, max_len)` | `size_t` | **零拷贝读取**：直接从 IDF DMA 缓冲区读取，不经过软件环形缓冲区 |

### 4.4 观察者槽位

| 方法 | 说明 |
|------|------|
| `signal_RxComplete(slot)` | RX 数据就绪回调，通过 `TaskReactor` 调度 |
| `signal_TxComplete(slot)` | TX 完成回调 |

---

## 5. Esp32UartDmaDriver — DMA 实现

文件: [uart_dma_driver.hpp](../components/bsp/include/uart_dma_driver.hpp) | [uart_dma_driver.cpp](../components/bsp/src/uart_dma_driver.cpp)

### 5.1 配置结构

```cpp
struct Config {
    uart_port_t uart_num         = UART_NUM_1;    // UART 端口号
    int         tx_pin           = -1;            // TX GPIO
    int         rx_pin           = -1;            // RX GPIO
    int         baudrate         = 115200;        // 波特率
    size_t      tx_buf_size      = 4096;          // IDF DMA TX 环形缓冲区大小
    size_t      rx_buf_size      = 4096;          // IDF DMA RX 环形缓冲区大小
    int         event_queue_size = 16;            // UART 事件队列深度 (0=禁用事件)
};
```

实际使用配置 (main.cpp):
- `uart_num = UART_NUM_0`, `tx_pin = GPIO11`, `rx_pin = GPIO12`, `baudrate = 115200`

### 5.2 关键成员

| 成员 | 类型 | 说明 |
|------|------|------|
| `rx_buf_` | `atomic_array<uint8_t, 512>` | 无锁环形缓冲区，轮询路径用 |
| `line_buf_` | `char[256]` | `drainLines()` 的临时缓冲区 |
| `uart_event_queue_` | `QueueHandle_t` | 原生 IDF UART 事件队列句柄 |
| `running_` | `atomic<bool>` | 运行状态标志 |

### 5.3 初始化流程 (`init()`)

```
1. uart_driver_install()    — 安装驱动，分配 DMA 缓冲区，创建事件队列
2. uart_param_config()      — 配置波特率、数据位、校验、停止位
3. uart_set_pin()           — 配置 TX/RX GPIO 引脚
   ↓ 任一步骤失败则回滚 (删除驱动, 置空事件队列句柄)
```

### 5.4 TX 数据路径 (零额外忙等)

```
write(data, len)
    └── uart_write_bytes()  →  IDF DMA 环形缓冲区  →  硬件 TX
        非阻塞，直接返回入队字节数

flush()
    └── uart_wait_tx_done(1000ms)
        阻塞等待 DMA FIFO 排空

tx_pending()
    └── uart_get_buffered_data_len()
        查询 DMA 缓冲区待发送数据量
```

### 5.5 RX 数据路径 (两套读取路径)

#### 路径 A: 事件驱动 (UartRxTask)

```
UART 硬件
  └─DMA→ IDF RX 环形缓冲区
            └─ISR→ uart_event_queue_ (UART_DATA 事件)
                      └─→ UartRxTask: xQueueReceive()
                            └─→ drainLines() → readAvailable()
                                  └─→ LineCallback(line)
```

#### 路径 B: 轮询读取

```
available()  → 从 IDF 缓冲区搬运到 rx_buf_ (atomic_array)
             → 返回可读字节数

read()       → 从 rx_buf_ 弹出数据 (无超时)
             → 或直接 uart_read_bytes() (有超时)

readAvailable() → uart_read_bytes() 直接读到用户缓冲区 (零拷贝)
```

### 5.6 drainLines() — 行数据排空

```cpp
bool drainLines(LineCallback on_line);
```

- 调用 `readAvailable()` 直接从 IDF DMA 缓冲区读取最多 255 字节
- 自动添加 `'\0'` 终止符
- 调用回调函数传递完整字符串 (不是逐行切割——每次交付硬件当前可用的全部数据)
- 返回值: `true` 表示有数据被处理

### 5.7 signal_RxComplete() — 反应器槽位

```cpp
void signal_RxComplete(function<void(const uint8_t*, size_t)> slot);
```

- 先将 IDF 缓冲区数据搬运到 `rx_buf_`
- 分块弹出数据 (每块 ≤256 字节)，逐块调用 `slot(buf, len)`
- 由 `TaskReactor` 的 `connect()` 机制调度执行

---

## 6. Logger — UART 后端日志系统

文件: [logger.hpp](../components/logger/include/logger.hpp) | [logger.cpp](../components/logger/src/logger.cpp)

### 6.1 两种集成路径

```
路径 1: 直接调用 API (类型安全, 编译期检查)
  logger.info("motor {} pos={:.3f}", id, pos);
    └── std::vformat() → writeImpl() → UART DMA TX

路径 2: ESP_LOGx 拦截 (printf 兼容, 捕获所有 IDF 日志)
  esp_log_set_vprintf(&Logger::vprintfHook)
    └── ESP_LOGx宏 → vsnprintf() → Logger::vprintfHook() → UART DMA TX
```

### 6.2 日志级别

```cpp
enum class LogLevel : uint8_t {
    NONE    = 0,   // 关闭所有输出
    ERROR   = 1,   // 错误
    WARN    = 2,   // 警告
    INFO    = 3,   // 信息 (默认级别)
    DEBUG   = 4,   // 调试
    VERBOSE = 5,   // 详细
};
```

全局级别可通过 `Logger::setGlobalLevel()` 动态设置，默认 `INFO`。

### 6.3 输出格式

每条日志的格式为：

```
[级别字符][标签] 消息体\r\n
```

级别字符映射: `E`=ERROR, `W`=WARN, `I`=INFO, `D`=DEBUG, `V`=VERBOSE

示例输出:
```
[I][motor] target_vel=0.5 rps, pos=1.234 rev, torque=150 ‰, temp=42.5°C
[I][UART_RX] spd 0 0.5
[I][serial_cmd] applied: motor0 vel=0.50 rps
```

### 6.4 静态成员

| 成员 | 类型 | 说明 |
|------|------|------|
| `s_uart_` | `IUartDriver*` | 全局 UART 输出通道 (单例) |
| `s_global_level_` | `LogLevel` | 全局日志级别过滤 |
| `s_write_mutex_` | `std::mutex` | 写互斥锁 (线程安全) |

### 6.5 writeImpl() — 核心输出

```
writeImpl(level, msg)
  ├── std::lock_guard 加锁
  ├── snprintf 构建日志头 [L][tag]
  ├── uart->write(header)
  ├── uart->write(msg)
  └── uart->write("\r\n")
```

### 6.6 vprintfHook — ESP_LOG 拦截

```cpp
static int vprintfHook(const char* fmt, va_list args);
```

- 无锁热路径（`setUart()` 仅在启动时调用一次，之后只读）
- 栈上 256 字节缓冲区，无堆分配
- 通过 `esp_log_set_vprintf()` 安装后，所有 `ESP_LOGI/E/W/D/V` 宏输出都经此路由

### 6.7 std::format 兼容性

- 编译器支持 `<format>` 时：启用 `std::vformat`，类型安全的格式化
- 不支持时：回退到纯字符串输出（格式化参数被丢弃），建议安装 `fmt` 库：
  ```
  idf.py add-dependency fmt
  ```
  然后将 `<format>` 替换为 `<fmt/format.h>`

---

## 7. UartRxTask — 串口命令接收任务

文件: [main.cpp](../main/main.cpp) (第 55-136 行)

### 7.1 任务配置

| 参数 | 值 |
|------|-----|
| 任务名 | `"uart_rx"` |
| 栈大小 | 2560 字节 |
| 优先级 | 10 |
| 阻塞方式 | `xQueueReceive(event_queue, portMAX_DELAY)` |

### 7.2 事件处理流程

```
UartRxTask 主循环
  │
  ├── xQueueReceive(uart->event_queue(), &event, portMAX_DELAY)
  │     └── 阻塞等待 IDF UART 事件
  │
  ├── UART_DATA:
  │     └── drainLines([&](line) {
  │           ├── parseStrCMD(line)       // 分离命令和参数
  │           ├── 匹配 "spd" 命令:
  │           │     ├── "spd ?" → 查询当前设定值
  │           │     ├── "spd <vel>" → 解析并发布速度指令 (atomic store + TaskNotifyGive)
  │           │     └── 速度钳位 ±3.0 rev/s (在 app_main 中执行)
  │           └── Logger 输出解析结果
  │         })
  │
  ├── UART_FIFO_OVF:   ESP_LOGW + uart_flush_input()
  ├── UART_BUFFER_FULL: ESP_LOGW
  └── UART_BREAK:       ESP_LOGI
```

### 7.3 支持的命令

#### spd — 设置/查询电机 0 目标速度

```
spd <velocity_rps>
spd ?
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `velocity_rps` | float | 目标速度 (Rev/s)，正=正转，负=反转 |

示例:
```
spd 2.5        → 电机0 以 2.5 Rev/s 运行
spd 0          → 电机0 停止
spd -1.0       → 电机0 以 1.0 Rev/s 反转
spd ?          → 查询当前设定
```

### 7.4 线程安全 — 命令发布

使用无锁原子变量 + FreeRTOS TaskNotify 实现生产者-消费者同步：

```cpp
// 共享状态 (main.cpp)
static std::atomic<float> g_target_velocity{0.0f};
static TaskHandle_t       g_app_main_handle = nullptr;

// UartRxTask 写入 (生产者)
g_target_velocity.store(vel, std::memory_order_relaxed);
xTaskNotifyGive(g_app_main_handle);     // 唤醒 app_main

// app_main 主循环消费 (消费者)
if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))) {
    float new_vel = g_target_velocity.load(std::memory_order_relaxed);
    // 限幅到 ±3.0 Rev/s
    if (new_vel >  kMaxVelocity) new_vel =  kMaxVelocity;
    if (new_vel < -kMaxVelocity) new_vel = -kMaxVelocity;
    motor0_velocity = new_vel;
}
```

> 设计要点：`g_target_velocity` 是 `std::atomic<float>`，`xTaskNotifyGive` / `ulTaskNotifyTake` 提供无锁唤醒机制，无需额外的互斥锁或条件变量。

---

## 8. 初始化流程 (app_main)

```
app_main()
  │
  ├── 1. 创建 Esp32UartDmaDriver (UART_NUM_0, TX=GPIO11, RX=GPIO12, 115200)
  │      ├── init() → uart_driver_install + param_config + set_pin
  │      └── start() → running_ = true
  │
  ├── 2. 安装 Logger UART 后端
  │      ├── Logger::setUart(&uart_drv)
  │      └── esp_log_set_vprintf(&Logger::vprintfHook)
  │         (此后所有 ESP_LOGx 输出均通过 UART DMA 发送)
  │
  ├── 3. 创建 UartRxTask
  │      └── xTaskCreate(UartRxTask, "uart_rx", 2560, &uart_drv, 10, ...)
  │         (阻塞在事件队列上等待 RX 数据)
  │
  ├── 4. 业务初始化 (CAN 驱动, 电机控制, GPIO)
  │
  └── 5. 初始化电机控制
         ├── 创建 HexfellowMotorTask 并 start()
         ├── waitForInit(1500) — 阻塞等待 CANopen SDO 初始化完成 (二进制信号量)
         └── 超时或失败 → 退出; 成功 → 进入主循环

  └── 6. 主循环 (事件驱动, ulTaskNotifyTake 500ms 超时)
         ├── 收到 spd 命令 → 更新电机 0 目标速度 (±3.0 rev/s 限幅)
         ├── hexmotor_task.setMitTarget(0, target)
         ├── 读取电机 0 状态快照
         └── Logger 输出电机状态
```

---

## 9. 数据流全览

```
┌─────────────────────────────────────────────────────────────────────┐
│                          日志输出 (TX)                               │
│                                                                     │
│  用户代码                  Logger                Esp32UartDmaDriver  │
│  ┌──────────┐  直接API   ┌──────────┐  write()  ┌──────────────────┐│
│  │logger.info│──────────▶│writeImpl │──────────▶│uart_write_bytes  ││
│  │("msg {}", │           │+ 互斥锁   │           │→ DMA ring buf   ││
│  │  arg)    │           │+ [L][tag]│           │→ UART HW TX     ││
│  └──────────┘           └──────────┘           └──────────────────┘│
│                               ▲                                     │
│  ┌──────────┐  ESP_LOGx     │                                      │
│  │ESP_LOGI  │───────────────┘                                      │
│  │("...")   │  vprintfHook()                                       │
│  └──────────┘                                                       │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                        命令接收 (RX)                                 │
│                                                                     │
│  UART HW RX                                                         │
│    └─DMA→ IDF RX ring buffer                                        │
│              └─ISR→ event_queue (UART_DATA)                         │
│                        │                                            │
│              ┌─────────▼──────────┐                                 │
│              │    UartRxTask       │                                 │
│              │  xQueueReceive()    │                                 │
│              │  drainLines(line):  │                                 │
│              │    parseStrCMD()    │                                 │
│              │    parseStrArg()    │                                 │
│              │    g_target_velocity│  (atomic + TaskNotify)          │
│              │    xTaskNotifyGive()│                                 │
│              └────────────────────┘                                 │
│                        │                                            │
│                        ▼                                            │
│              ┌────────────────────┐                                 │
│              │  app_main 主循环    │                                 │
│              │  ulTaskNotifyTake() │                                 │
│              │  → 读取 atomic 速度  │  (无锁)                        │
│              │  → setMitTarget()   │                                 │
│              └────────────────────┘                                 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 10. 设计原则与要点

### 10.1 零额外 FreeRTOS 原语

IDF 原生的 UART 事件队列就是通知机制——不需要额外的信号量、消息队列或任务来桥接 ISR。用户任务直接 `xQueueReceive()` 到 IDF 事件队列句柄。

### 10.2 零中间拷贝 (RX 热路径)

`readAvailable()` 和 `drainLines()` 直接从 IDF DMA 环形缓冲区读取数据到目标缓冲区，不经过 `rx_buf_` 软件环形缓冲区。`rx_buf_` 仅用于轮询路径 (`available()` / `read()`)。

### 10.3 无锁 RX 缓冲区

`rx_buf_` 基于 `atomic_array` 实现——SPSC (单生产者单消费者) 无锁环形缓冲区，使用 C++ `memory_order` 保证多线程正确性，无需互斥锁。

### 10.4 线程安全日志

`Logger::writeImpl()` 使用 `std::mutex` 保护 UART 写操作，确保多任务日志输出不会交错。`vprintfHook()` 在热路径上无锁（因为 `s_uart_` 指针仅在启动时写入一次）。

### 10.5 观察者模式支持

`IUartDriver` 继承 `BasicObject`，通过 `bindReactor()` 可与 `TaskReactor` 集成，实现信号-槽模式的异步事件分发。`signal_RxComplete` 和 `signal_TxComplete` 是预定义的槽位。

## 11. 扩展指南

### 12.1 添加新的串口命令

在 `UartRxTask` 的 `drainLines` 回调中添加新的命令分支：

```cpp
if (strCmd.command == "mycmd") {
    // 解析参数
    int param = 0;
    if (!TaskReactor::parseStrArg(strCmd.args, param)) {
        rx_log.warn("[CMD] bad param in: {}", line);
        return;
    }
    // 执行业务逻辑 (注意线程安全)
    rx_log.info("[CMD] mycmd executed with param={}", param);
}
```

### 12.2 替换/增加 UART 端口

修改 `Esp32UartDmaDriver::Config`：
```cpp
uart_cfg.uart_num = UART_NUM_1;   // 使用第二个 UART
uart_cfg.tx_pin   = GPIO_NUM_17;  // 新的 TX 引脚
uart_cfg.rx_pin   = GPIO_NUM_18;  // 新的 RX 引脚
```

### 12.3 实现新的 UART 驱动

继承 `IUartDriver` 并实现全部虚函数，可替换 `Esp32UartDmaDriver`（例如使用中断模式而非 DMA，或适配其他硬件平台）。`Logger` 和 `UartRxTask` 通过 `IUartDriver*` 接口操作，无需改动。

---

*文档最后更新: 2026-06-18*
