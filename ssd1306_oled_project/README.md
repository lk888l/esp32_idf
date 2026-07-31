# SSD1306 OLED project

这是一个面向 ESP32-C5 的 C++20 ESP-IDF 示例项目，驱动常见的 0.96 寸、128×64、
SSD1306 I²C OLED。显示使用固定版本的 U8g2 C API 和全屏 framebuffer，应用层沿用
`esp_idf_template` 的模块与任务生命周期。

## 硬件连接

| OLED 引脚 | ESP32-C5 | 说明 |
| --- | --- | --- |
| VCC | 3.3V | 请先确认模块支持 3.3V |
| GND | GND | 共地 |
| SDA | GPIO8 | I²C0 数据线 |
| SCL | GPIO9 | I²C0 时钟线 |

默认使用 7-bit 地址 `0x3C` 和 400 kHz 时钟。板级常量位于
`components/bsp/include/board_config.hpp`。驱动会启用 ESP32-C5 内部上拉，但为了获得
更可靠的高速通信，建议模块或总线上安装合适的外部上拉电阻。

## 演示内容

屏幕每秒刷新一次，每三秒依次切换：

1. 状态页：运行时间、空闲堆、刷新计数和一分钟进度条。
2. 图形页：矩形、圆、三角形、圆弧和进度条。
3. 灰度页：使用稳定的 4×4 Bayer 空间抖动模拟中间亮度。

SSD1306 是 1-bit 单色屏，灰度效果不是硬件灰度，也不使用可能产生闪烁的时间 PWM。

## 项目结构

```text
ssd1306_oled_project/
|- main/                    OLED 模块、演示任务和 app_main
|- components/
|  |- app/                  模块管理与 FreeRTOS 任务封装
|  |- base/                 模板基础工具
|  |- bsp/                  ESP-IDF 新版 I²C master BSP
|  |- display/              SSD1306 与跨平台绘图 API
|  |- logger/               日志封装
|  `- u8g2/                 固定版本 U8g2 源码与许可证
|- sdkconfig.defaults
`- CMakeLists.txt
```

## 构建与烧录

在 PowerShell 中加载本机 ESP-IDF 5.5.4 环境：

```powershell
$env:Path = "C:\kk_software\toolchain\esp_idf\python_env\idf5.5_py3.11_env\Scripts;$env:Path"
Set-ExecutionPolicy -Scope Process Bypass -Force
& C:\kk_software\toolchain\esp_idf\frameworks\esp-idf-v5.5.4\export.ps1
cd C:\kk_data\code\esp32\esp32_idf\ssd1306_oled_project
idf.py set-target esp32c5
idf.py build
```

执行策略只对当前 PowerShell 进程生效，不会修改系统级策略。若使用已经完成环境初始化的
ESP-IDF 终端，可以直接从 `cd` 和 `idf.py` 命令开始。

烧录并打开监视器：

```powershell
idf.py -p COMx flash monitor
```

退出串口监视器使用 `Ctrl+]`。

## 黑屏与通信错误排查

- 检查 VCC、GND、SDA、SCL 是否接反，并确认 OLED 与 ESP32 共地。
- 扫描或核对模块地址；部分模块可能使用 `0x3D`，此时修改 `kOledI2cAddress`。
- 线长、上拉不足或 400 kHz 不稳定时，将 `kOledI2cClockHz` 暂时降到 `100000`。
- 确认模块控制器确实是 SSD1306；外观相同的 SH1106 需要不同的 U8g2 setup。
- 启动日志出现 `transport_error` 通常表示地址错误、模块未供电或总线未正确连接。
- 若图像偏移或裁剪，确认屏幕分辨率是 128×64，而不是 128×32。

程序初始化失败时会记录错误并停留在安全循环中，不会保留无效的 I²C 设备句柄。
