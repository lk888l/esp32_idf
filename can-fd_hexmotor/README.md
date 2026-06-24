# CAN-FD Hexfellow Motor Controller

基于 **ESP32-C5** + **ESP-IDF 5.5** 的多电机 CANopen-over-CAN-FD 实时控制系统。通过一对多 RPDO 广播架构，以 **1ms 控制周期**同时驱动最多 **8 台 Hexfellow 无刷直流电机**。

如果你只想简单使用（无需 PDO 广播架构），可以参考 [SDO 简易驱动演示](#sdo-简易驱动演示-motor_sdo_control_task) —— 一个独立的 FreeRTOS 任务，仅通过 CANopen SDO expedited download 即可配置并驱动电机，需要注意：在一个单片机中请选择一个任务运行，PDO（Motor_Control_Task类）或SDO（独立任务函数Motor_Control_Task），切忌不要同时运行两演示任务，否则会导致冲突。

!!! warning "仅供演示"
    本工程**仅用于学习与台架验证**，非生产级软件；无完善错误恢复与安全联锁。用于任何可能造成伤害或损失的场景前，请自行审计并编写适合产品的驱动。

---

## 目录

- [硬件需求](#硬件需求)
- [软件架构](#软件架构)
- [快速开始](#快速开始)
- [引脚定义](#引脚定义)
- [串口命令](#串口命令)
- [项目结构](#项目结构)
- [组件说明](#组件说明)
- [控制模式](#控制模式)
- [PDO 帧布局](#pdo-帧布局)
- [配置参数](#配置参数)
- [HexfellowMotorTask 类(PDO任务)](#hexfellowmotortask-类pdo任务)
- [SDO 简易驱动演示 (Motor_SDO_Control_Task)](#sdo-简易驱动演示-motor_sdo_control_task)
- [技术文档](#技术文档)

---

## 硬件需求

| 组件 | 规格 |
|------|------|
| **主控芯片** | ESP32-C5 |
| **CAN-FD 收发器** | 外接 CAN-FD 收发器（TX=GPIO4, RX=GPIO5） |
| **电机** | Hexfellow 系列无刷直流电机（固件 v8~v9），最多 8 台 |
| **终端电阻** | **终端电阻 120Ω** |
| **串口** | UART0（TX=GPIO11, RX=GPIO12），用于日志输出和命令输入 |

### CAN-FD 总线参数

| 参数 | 值 |
|------|-----|
| 仲裁域波特率 | 1 Mbps |
| 数据域波特率 | 5 Mbps |
| 仲裁域采样点 | 80% |
| 数据域采样点 | 75% |

---

## 软件架构

```
┌─────────────────────────────────────────────────────────────┐
│                       app_main (主循环)                       │
│  • 事件驱动：ulTaskNotifyTake(500ms) 等待 UartRxTask 唤醒      │
│  • 接收 spd 命令（原子变量 + TaskNotify），更新电机 0 MIT 目标速度  │
├─────────────────────────────────────────────────────────────┤
│  HexfellowMotorTask (1ms FreeRTOS 任务, 优先级 8)             │
│  • 主站心跳 50ms / RPDO 广播 1ms / TPDO 接收                   │
│  • MIT 模式：位置/速度/扭矩/Kp/Kd 5 维阻抗控制                  │
│  • Profile Velocity 模式：速度控制 + 扭矩限幅                    │
│  • 二进制信号量 notify: CANopen SDO 初始化完成 → app_main 唤醒   │
├─────────────────────────────────────────────────────────────┤
│  协议层: co_master_sdo (CANopen SDO 主站客户端)                │
│  • 电机初始化序列：NMT → 参数配置 → PDO 映射 → CiA402 状态机     │
│  • 支持 expedited download/upload (u8/u16/u32/i8/i32/f32)     │
├───────────────┬─────────────────────────────────────────────┤
│   BSP 驱动层   │  CAN-FD: Esp32CanFdDriver                    │
│               │  UART:  Esp32UartDmaDriver (DMA)              │
├───────────────┴─────────────────────────────────────────────┤
│  基础组件: BasicObject / TaskReactor / atomic_array           │
│  软件框架: AppTask / AppModule / AppManager (可扩展)           │
└─────────────────────────────────────────────────────────────┘
```

### 数据流

```
串口命令 "spd <vel>"
    │
    ▼
UartRxTask (阻塞等待 IDF UART 事件队列)
    │ 解析 → 写入 g_target_velocity (std::atomic<float>) + xTaskNotifyGive()
    ▼
app_main 主循环 (事件驱动, 500ms 超时)
    │ ulTaskNotifyTake() 唤醒 → 读取 g_target_velocity → hexmotor_task.setMitTarget()
    ▼
HexfellowMotorTask (1ms)
    │ 构建 RPDO 帧 → CAN-FD 广播 (COB-ID 0x190)
    ▼
Hexfellow 电机 (Node-ID 1~8)
    │ TPDO1 (1ms 反馈) / TPDO2 (20ms 状态) → CAN-FD 接收
    ▼
HexfellowMotorTask::handleRxFrame()
    │ 解析位置/速度/扭矩/温度/错误码
    ▼
主循环 snapshot() → 日志输出 (每个唤醒周期)
```

---

## 快速开始

### 1. 环境准备

- 安装 ESP-IDF 5.5

### 2. 克隆项目

```bash
git clone <repository-url>
cd can-fd_hexmotor
```

### 3. 配置项目

```bash
idf.py set-target esp32c5
idf.py menuconfig   # 按需调整 FreeRTOS / 调试选项

# ！！！需要注意：该工程修改了默认的主任务堆栈大小（改为8kb）与freeRTOS调度频率(改为1000)
```

> 项目已提供 `sdkconfig`，可直接使用默认配置。

### 4. 编译 & 烧录

```bash
idf.py build                         # 编译
idf.py -p <COM_PORT> flash           # 烧录
idf.py -p <COM_PORT> monitor         # 串口监控（日志 + 命令）
```

---

## 引脚定义

| 功能 | GPIO | 说明 |
|------|------|------|
| CAN-FD TX | GPIO4 | 连接外部 CAN-FD 收发器 TXD |
| CAN-FD RX | GPIO5 | 连接外部 CAN-FD 收发器 RXD |
| UART TX | GPIO11 | 日志输出（115200, 8N1） |
| UART RX | GPIO12 | 命令输入（115200, 8N1） |
| LED | GPIO25 | 活动指示灯（开漏输出，约 5Hz 闪烁） |

---

## 串口命令

通过 UART0（GPIO12 RX）发送文本命令，由 `UartRxTask` 逐行解析。

### `spd` — 速度控制

```bash
spd <velocity_rps>              # 设置电机 0 目标速度（转/秒）
spd ?                           # 查询当前设定值
```

**示例：**

```bash
spd 2.5       # 电机 0 → 2.5 rev/s
spd 0         # 电机 0 → 停止
spd -1.0      # 电机 0 → 反向 1.0 rev/s
spd ?         # 输出当前 velocity
```

> **注意**：当前仅控制电机 0，速度自动钳位在 ±3.0 rev/s。

---



## 项目结构

```
can-fd_hexmotor/
├── CMakeLists.txt                          # 根构建文件（C++20, 项目版本 1.0.0）
├── sdkconfig                               # ESP-IDF 项目配置
├── README.md                               # 本文件
├── main/
│   ├── CMakeLists.txt                      # 主程序依赖：logger, bsp, protocol, hardware
│   └── main.cpp                            # 入口：app_main, UartRxTask, 主循环
├── components/
│   ├── base/                               # 基础组件层
│   │   └── include/
│   │       ├── BasicObject.hpp             # FreeRTOS TaskNotify 信号发射基类
│   │       ├── TaskReactor.hpp             # 信号-槽分派器 + 串口命令解析
│   │       └── atomic_array.hpp            # 无锁 SPSC 环形缓冲区
│   ├── bsp/                                # 板级支持包（硬件驱动）
│   │   ├── include/
│   │   │   ├── canfd_driver.hpp            # Esp32CanFdDriver 声明
│   │   │   ├── canfd_frame.hpp             # CAN-FD 帧结构 + ICanDriver 接口
│   │   │   ├── uart_dma_driver.hpp         # Esp32UartDmaDriver 声明
│   │   │   └── uart_driver.hpp             # IUartDriver 抽象接口
│   │   └── src/
│   │       ├── canfd_driver.cpp            # CAN-FD TWAI 驱动实现
│   │       └── uart_dma_driver.cpp         # DMA UART 驱动实现
│   ├── logger/                             # 日志子系统
│   │   ├── include/logger.hpp              # Logger 类（std::format 风格）
│   │   └── src/logger.cpp                  # Logger 实现 + vprintfHook
│   ├── protocol/                           # 协议栈
│   │   └── canopen/
│   │       ├── canopen_sdo.hpp             # SDO 主站客户端声明
│   │       └── canopen_sdo.cpp             # SDO download/upload 实现
│   ├── hardware/                           # 
│   │   └── motor/
│   │       ├── hexfellow_motor_controller.hpp  # 多电机控制器
│   │       ├── hexfellow_motor_controller.cpp  # 初始化 / 控制循环 / PDO 映射
│   │       ├── hexfellow_motor_task.hpp        # FreeRTOS 任务包装器
│   │       └── hexfellow_motor_task.cpp        # 1ms 控制任务实现
│   └── app/                                # 应用框架
│       ├── include/
│       │   ├── app_manager.hpp             # AppManager 单例
│       │   ├── app_module.hpp              # AppModule 抽象基类
│       │   └── app_task.hpp                # AppTask FreeRTOS 任务包装器
│       └── src/
│           ├── app_manager.cpp             # 模块生命周期管理
│           └── app_task.cpp                # 任务创建/入口
└── docs/
    ├── esp32-c5_hexmotor_control.md         # PDO 映射与架构详细文档
    └── uart_subsystem.md                    # UART 子系统详细文档
```

---

## 组件说明

### `base` — 基础组件

| 类 | 功能 |
|----|------|
| `BasicObject` | 通过 FreeRTOS TaskNotify 发送信号（`emit` / `emitFromISR`） |
| `TaskReactor` | 信号-槽分派器：32 个槽位映射到 32 个通知位；提供 `parseStrCMD` / `parseStrArg` 串口命令解析 |
| `atomic_array<T,N>` | 无锁 SPSC 环形缓冲区，用于 ISR 与任务间的安全数据传输 |

### `bsp` — 板级驱动

| 类 | 功能 |
|----|------|
| `ICanDriver` | CAN(-FD) 抽象驱动接口，继承 `BasicObject` |
| `Esp32CanFdDriver` | 基于 ESP-IDF TWAI on-chip 的 CAN-FD 驱动：ISR 接收 → 原子环形缓冲区 → 信号通知 |
| `IUartDriver` | UART 抽象接口（TX/RX 生命周期 + 观察者信号） |
| `Esp32UartDmaDriver` | DMA UART 驱动：IDF 原生事件队列、零拷贝 RX、非阻塞 TX |

### `logger` — 日志

| 类 | 功能 |
|----|------|
| `Logger` | 线程安全的 `std::format` 风格日志器。双路径集成：直接 API 调用 + `esp_log_set_vprintf` 钩子拦截所有 `ESP_LOGx` 宏输出。日志格式：`[级别][标签] 消息\r\n` |

### `protocol/canopen` — CANopen 协议

| 类 | 功能 |
|----|------|
| `co_master_sdo` | CANopen SDO 主站客户端。支持 expedited download/upload（u8/u16/u32/i8/i32/f32），阻塞等待从站响应，超时/错误处理 |

### `hardware/motor` — 电机控制

| 类 | 功能 |
|----|------|
| `HexfellowMotorController` | 多电机控制器核心：初始化序列、PDO 映射配置、MIT/Velocity 目标设置、RPDO 帧构建、TPDO 帧解析、状态快照 |
| `HexfellowMotorTask` | FreeRTOS 任务包装：1ms 控制循环、主站心跳、RPDO 广播、CAN RX 信号重绑定、二进制信号量反馈初始化完成 |

### `app` — 应用框架

| 类 | 功能 |
|----|------|
| `AppTask` | 抽象 FreeRTOS 任务包装器（create / start / stop 生命周期） |
| `AppModule` | 抽象生命周期模块接口 |
| `AppManager` | 单例模块注册表与生命周期协调器（预留扩展） |

---

## 控制模式

系统支持两种 CiA402 工作模式，不同电机可独立配置：

| 模式 | CiA402 值 | 说明 |
|------|----------|------|
| **MIT** | `5` | 位置 / 速度 / 扭矩 / Kp / Kd 五维阻抗控制，每电机 8 字节 |
| **Profile Velocity** | `3` | 速度控制 + 扭矩限幅，每电机 6 字节 |

### MIT 目标量化

5 个浮点值按照以下默认范围线性量化为 64 位紧凑格式：

| 参数 | 位宽 | 默认范围 | 精度 |
|------|------|---------|------|
| 位置 (position) | 16 bit | ±0.5 rev | ~0.000015 rev |
| 速度 (velocity) | 12 bit | ±10 rev/s | ~0.0049 rev/s |
| 扭矩 (torque) | 12 bit | ±10 Nm | ~0.0049 Nm |
| Kp | 12 bit | 0 ~ 100 Nm/rev | ~0.024 Nm/rev |
| Kd | 12 bit | 0 ~ 20 Nm·s/rev | ~0.0049 Nm·s/rev |

---

## PDO 帧布局

### RPDO1 — 控制帧（主站 → 电机, COB-ID: `0x190`）

CAN-FD 64 字节帧，MIT 模式顺序紧密排列：

```
Byte 0-7:   电机 0 MIT 数据
Byte 8-15:  电机 1 MIT 数据
           ...
Byte 56-63: 电机 7 MIT 数据
```

> 每电机 8 字节（MIT 模式）或 6 字节（Velocity 模式），可混用。

### TPDO1 — 高频反馈帧（12 字节, COB-ID: `0x180 + Node-ID`）

| 偏移 | 长度 | 内容 |
|------|------|------|
| Byte 0-3 | 4B | 实际位置 REAL32 (rev) |
| Byte 4-7 | 4B | 高速时间戳 UINT32 (μs) |
| Byte 8-9 | 2B | 实际扭矩 INT16 (‰ 额定扭矩) |
| Byte 10-11 | 2B | 错误码 UINT16 |

- 发送周期: ~1ms（事件驱动）
- 抑制时间: 0.5ms

### TPDO2 — 状态/诊断帧（8 字节, COB-ID: `0x280 + Node-ID`）

| 偏移 | 长度 | 内容 |
|------|------|------|
| Byte 0-3 | 4B | 状态寄存器 + 控制字回显 |
| Byte 4-5 | 2B | 电机温度 INT16 (℃ × 10) |
| Byte 6-7 | 2B | 母线电压 UINT16 (V × 10) |

- 发送周期: ~20ms（事件驱动）

---

## 配置参数

### 主站配置（`hexfellow_motor_controller.hpp`）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `MASTER_NODE_ID` | `0x10` | 主站 CANopen Node-ID |
| `HEXFELLOW_RPDO_COB_ID` | `0x190` | 共享 RPDO COB-ID |
| `MAX_MOTORS` | `8` | 最大电机数量 |
| `MASTER_HB_PERIOD_MS` | `50` | 主站心跳周期 |
| `MOTOR_HB_TIMEOUT_MS` | `500` | 电机心跳超时 |
| `TPDO1_INHIBIT_X100US` | `5` | TPDO1 抑制时间 (0.5ms) |
| `TPDO1_EVENT_MS` | `1` | TPDO1 事件定时器 |
| `TPDO2_INHIBIT_X100US` | `190` | TPDO2 抑制时间 (19ms) |
| `TPDO2_EVENT_MS` | `20` | TPDO2 事件定时器 |

### 电机任务配置（`hexfellow_motor_task.hpp`）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `RPDO_PERIOD_MS` | `1` | RPDO 发送周期 |
| `INIT_TIMEOUT_MS` | `1500` | 电机 CANopen 初始化超时 (app_main 中 waitForInit) |
| Task stack size | `8192` | 任务栈大小 (bytes) |
| Task priority | `8` | FreeRTOS 任务优先级 |

### 日志配置（`logger.hpp`）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 默认日志级别 | `INFO` (3) | 可选: NONE/ERROR/WARN/INFO/DEBUG/VERBOSE |
| 栈缓冲区 | `256` bytes | vprintfHook 行缓冲 |

---
## HexfellowMotorTask 类(PDO任务)

`HexfellowMotorTask` 继承自 `AppTask`，是本项目的核心控制任务，以 **1ms 周期**通过 RPDO 广播驱动多台 Hexfellow 电机。

### 主要接口

| 方法 | 说明 |
|------|------|
| `initMotors()` | 通过 CANopen SDO 执行电机初始化序列（NMT → 参数配置 → PDO 映射 → CiA402 状态机） |
| `waitForInit(timeout_ms)` | 阻塞等待初始化完成，超时返回 `false` |
| `setMitTarget(index, target)` | 设置指定电机的 MIT 模式目标（位置/速度/扭矩/Kp/Kd） |
| `setVelocityTarget(index, rev_s, torque_‰)` | 设置指定电机的 Profile Velocity 模式目标 |
| `snapshot(index, out)` | 获取指定电机的最新状态快照（位置/速度/扭矩/温度/错误码） |

### 构造参数

```cpp
HexfellowMotorTask(
    const std::string& name,           // 任务名称
    uint32_t stack_size,               // 栈大小（默认 8192）
    UBaseType_t priority,              // FreeRTOS 优先级（默认 8）
    Esp32CanFdDriver& driver,          // CAN-FD 驱动引用
    const HexfellowMotorController::Config& cfg,  // 电机控制器配置
    BaseType_t core = tskNO_AFFINITY   // 核心亲和性
);
```

### 控制循环

任务主循环 (`main()`) 执行以下逻辑：
1. 每 **50ms** 发送一次主站心跳
2. 每 **1ms** 构建并广播 RPDO 帧（COB-ID `0x190`）
3. 通过 CAN RX 信号接收 TPDO 反馈帧并解析电机状态
4. 初始化完成后通过二进制信号量通知 `app_main`

### 使用示例

```cpp
// 创建任务
HexfellowMotorTask hexmotor_task("hexmotor", 8192, 8, can_driver, motor_cfg);

// 初始化电机
hexmotor_task.initMotors();
if (!hexmotor_task.waitForInit(1500)) {
    ESP_LOGE(TAG, "Motor init failed!");
    return;
}

// 设置目标速度
hexmotor_task.setVelocityTarget(0, 2.5f, 200);

// 获取状态
HexfellowMotorController::MotorState state;
hexmotor_task.snapshot(0, state);
```

---
## SDO 简易驱动演示 (Motor_SDO_Control_Task)

`Motor_SDO_Control_Task` 是一个独立的 FreeRTOS 任务，演示如何**仅通过 SDO** 驱动一台 Hexfellow 电机，无需 PDO 映射或 `HexfellowMotorTask`。

> **注意**：该任务与 `HexfellowMotorTask` 互斥，不要同时使用。当前在 `app_main()` 中已被注释掉，如需启用，取消注释 `xTaskCreate` 调用即可。

### 流程

1. 初始化 CAN-FD 驱动（仲裁域 1Mbps / 数据域 5Mbps）
2. 绑定 CAN RX 完成信号到 FreeRTOS 任务通知
3. 创建 SDO 主站客户端
4. 通过 SDO expedited download 依次写入：工作模式 → 目标速度 → 控制字（Shutdown → Switch On → Enable）→ 最大扭矩
5. 主循环持续接收并打印 CAN 帧

### 代码

```cpp
/**
 * @brief Motor control task — 纯 SDO 方式驱动电机演示
 */
void Motor_SDO_Control_Task(void* pvParameters)
{
    // TODO: 当前使用固定通知位，后续可改为运行时分配
    constexpr uint32_t CAN_RX_NOTIFY_BIT = (1 << 2);

    // ── 1. 初始化 CAN-FD 驱动 ──────────────────────────────
    Esp32CanFdDriver::Config can_cfg = {};
    can_cfg.tx_pin               = GPIO_NUM_4;
    can_cfg.rx_pin               = GPIO_NUM_5;
    can_cfg.arbitration_bitrate  = 1000000;   // 1 Mbps
    can_cfg.data_bitrate         = 5000000;   // 5 Mbps
    Esp32CanFdDriver can_driver(can_cfg);
    if (!can_driver.init()) {
        ESP_LOGI(MOTOR_TAG, "Failed to initialize CAN driver");
        return;
    }
    if (!can_driver.start()) {
        ESP_LOGI(MOTOR_TAG, "Failed to start CAN driver");
        return;
    }

    // ── 2. 绑定 CAN RX 信号 ─────────────────────────────────
    can_driver.bindReactor(&Esp32CanFdDriver::signal_RxComplete,
                           xTaskGetCurrentTaskHandle(),
                           CAN_RX_NOTIFY_BIT);

    // ── 3. 创建 SDO 主站 ────────────────────────────────────
    co_master_sdo sdo(can_driver, CAN_RX_NOTIFY_BIT);

    uint8_t target_node = 0x01;  // 假设电机从站 Node-ID = 1

    // ── 4. SDO 配置序列 ─────────────────────────────────────
    int res = sdo.dl_i8(target_node,  0x6060, 0x00, 3);        // 工作模式 → Profile Velocity
    res |= sdo.dl_i32(target_node,   0x60FF, 0x00, 0, 100);    // 目标速度 = 0
    res |= sdo.dl_u16(target_node,   0x6040, 0x00, 6, 100);    // 控制字 → Shutdown
    res |= sdo.dl_u16(target_node,   0x6072, 0x00, 200, 100);  // 最大扭矩 = 200‰
    res |= sdo.dl_u16(target_node,   0x6040, 0x00, 7, 100);    // 控制字 → Switch On
    res |= sdo.dl_u16(target_node,   0x6040, 0x00, 0x0f, 100); // 控制字 → Enable Operation
    res |= sdo.dl_f32(target_node,   0x60FF, 0x00, 1.0f, 100); // 目标速度 = 1.0 rev/s

    if (res != static_cast<int>(co_master_sdo::SdoResult::OK)) {
        ESP_LOGE(MOTOR_TAG, "Write Modes of operation failed: %s",
                 sdo.strerr(static_cast<co_master_sdo::SdoResult>(res)));
    } else {
        ESP_LOGI(MOTOR_TAG, "Write Modes of operation successfully.");
    }

    // ── 5. 主循环：接收并打印 CAN 帧 ───────────────────────
    while (true) {
        while (!can_driver.emply_rx_buffer()) {
            bsp::canfd::Frame frame;
            if (can_driver.pop_rx_buffer(frame)) {
                ESP_LOGI(MOTOR_TAG,
                         "Received CAN frame in motor control task. ID: 0x%08X, DLC: %d",
                         frame.id, frame.dlc);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Create a task for the motor control function
BaseType_t xReturn = pdPASS;
xReturn = xTaskCreate((TaskFunction_t)Motor_Control_Task,
                    (const char*)"MotorControl",
                    8192,
                    (void*)NULL,
                    (UBaseType_t)8,
                    (TaskHandle_t*)&Handle_MotorControlFunc);
if (xReturn != pdPASS) {
    logger.info("Failed to create motor control task");
    return;
}
```

### SDO 寄存器说明

| 寄存器 | 子索引 | 写入值 | 含义 |
|--------|--------|--------|------|
| `0x6060` | `0x00` | `3` | 工作模式 = Profile Velocity |
| `0x60FF` | `0x00` | `0` → `1.0` | 目标速度 (rev/s) |
| `0x6040` | `0x00` | `6` → `7` → `0x0F` | 控制字：Shutdown → Switch On → Enable |
| `0x6072` | `0x00` | `200` | 最大扭矩限制 (‰) |

---

## 技术文档

- [PDO 映射与架构文档](docs/esp32-c5_hexmotor_control.md) — CANopen COB-ID 分配、TPDO/RPDO 帧格式、MIT 量化算法、初始化序列、控制循环详解
- [UART 子系统文档](docs/uart_subsystem.md) — 硬件接口、架构图、DMA 数据流、类继承关系、命令解析协议、线程安全模型

