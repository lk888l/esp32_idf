# stepmotor_driver

面向 ESP32-C3 SuperMini、TB6612FNG 双 H 桥和 MT6816 编码器的两相步进电机实验工程。项目使用 C++17 与组件化分层，将应用生命周期、板级驱动、器件协议和电机矢量算法隔离。

当前实现是 **TB6612 电压模式正弦微步 / 开环电压矢量控制基础**，不是带相电流反馈的完整 FOC。TB6612 没有恒流斩波和相电流采样，因此软件幅值只代表 PWM 电压比例，不能解释成真实电机电流。

## 当前安全策略

- `kOpenLoopDemoEnabled=false`，默认只读取 MT6816，不给电机励磁。
- TB6612 的 `STBY` 接 GPIO10；初始化时首先保持低电平，方向和 PWM 全部配置完成后才允许使能。
- `disable()` 首先拉低 `STBY`，然后清零两路 PWM 和四个方向输出。
- 开环演示默认幅值为 `100/1000`，即峰值约 10% PWM；这仍不代表 100 mA。
- 未测量线圈电阻、电感、VM 电压、堵转电流和芯片温升前，不应提高幅值。

## 接线

### TB6612FNG

| TB6612 信号 | ESP32-C3 GPIO | 用途 |
|---|---:|---|
| PWMA | 0 | A 相 20 kHz PWM |
| PWMB | 1 | B 相 20 kHz PWM |
| AIN1 | 3 | A 相方向 1 |
| AIN2 | 8 | A 相方向 2；也会驱动 SuperMini 板载 LED |
| BIN1 | 20 | B 相方向 1；不再用作 UART RX |
| BIN2 | 21 | B 相方向 2；不再用作 UART TX |
| STBY | 10 | 功率级总使能，低有效停机 |
| AO1 / AO2 | — | 接步进电机 A 相线圈 |
| BO1 / BO2 | — | 接步进电机 B 相线圈 |
| VCC | 3.3 V | 逻辑电源 |
| VM | 电机电源 | 按模块、线圈及 TB6612 额定值选择 |
| GND / PGND | GND | 必须与 ESP32-C3 共地 |

如果所用 TB6612 模块把 `STBY` 硬件上拉或直接接高，软件将无法保证复位期间停机，应取消该连接并由 GPIO10 控制。VM/VCC 旁的去耦电容应靠近驱动板。

### MT6816

| MT6816 信号 | ESP32-C3 GPIO |
|---|---:|
| SCLK | 4 |
| MISO | 5 |
| MOSI | 6 |
| CS | 7 |

SPI 使用 SPI2、4 MHz、Mode 1。

## 引脚限制

- GPIO8 是 ESP32-C3 启动配置脚，并连接 SuperMini 板载 LED；这里只把它作为 AIN2，GPIO9 启动按键保持不占用。
- GPIO20/21 已用于 TB6612，主控制台已改为 USB Serial/JTAG，不能再通过 UART0 接收日志。
- ESP32-C3 ROM 在应用启动前仍可能在 GPIO21 输出启动信息，但 TB6612 的 `STBY` 内部下拉会保持功率级关闭。不要把 `STBY` 硬接高。
- 当前引脚已经不能同时直接增加 TWAI；后续需要重新分配编码器接口、增加 IO 扩展器或更换引脚更充足的主控板。

所有引脚集中定义在 [`components/bsp/include/board_config.hpp`](components/bsp/include/board_config.hpp)，并有编译期重复检查和 USB 控制台检查。

## 软件结构

```text
main/                 组合并启动应用模块
components/app/       AppModule / AppManager / FreeRTOS 任务生命周期
components/app_modules/
                      TB6612、MT6816、矢量算法的装配与运行策略
components/bsp/       ESP32-C3 TB6612 和 SPI 实现
components/hardware/  MT6816 器件协议
components/motor_control/
                      与 ESP-IDF 无关的正弦电压矢量算法
components/base/      通用 C++ 基础类型
components/logger/    日志封装
tests/                Windows 主机算法测试
doc/architecture.md   详细设计与闭环演进路线
```

## 构建与烧录

```powershell
cd C:\kk_data\code\esp32\esp32_idf\stepmotor_driver
idf.py -B build_refactored set-target esp32c3 build
idf.py -B build_refactored -p COMx flash monitor
```

监视器对应 ESP32-C3 原生 USB Serial/JTAG。这里使用新的 `build_refactored/`，避免旧 demo 的 `build/` 缓存。

## 开环验证

确认接线、VM、限流风险及线圈配对后，才可将：

```cpp
inline constexpr bool kOpenLoopDemoEnabled = false;
```

改为 `true`。`kDemoAmplitudePermille=100` 表示正弦峰值为最大 PWM 的 10%，电角度默认每 1 ms 前进 1/1024 周期。应从低幅值、短时间开始，并同时测量线圈电流和驱动温度。

架构和安全时序见 [架构说明](doc/architecture.md)。正弦矢量及 MT6816 协议参考 [Dummy-Robot Ctrl-Step-Driver-STM32F1-fw](https://github.com/peng-zhihui/Dummy-Robot/tree/main/2.Firmware/Ctrl-Step-Driver-STM32F1-fw)，功率输出则已按 TB6612 接口重新实现。

