# M5-StickS3 Motion UI

这是一个基于 ESP-IDF 5.5、LVGL 9 和 VQF 的 M5-StickS3 工程。工程沿用
`esp_idf_template` 的 `AppModule` / `AppManager` / `AppTask` 分层方式，并参考
`esp-idf-icm42688` 的 100 Hz 传感器采样与 VQF 6D 姿态解算流程。

> StickS3 板载 IMU 是 **BMI270**，不是 ICM42688。工程使用 Espressif 官方
> `espressif/bmi270` 驱动，并复用参考工程中的 VQF 实现。

## 已实现

- ST7789P3 135×240 彩屏，使用原生 135×240 竖屏扫描方向
- 40 MHz SPI、RGB565、双 DMA 行缓冲
- M5PM1 L3B 屏幕电源控制和 PWM 背光渐亮
- LVGL 卡片式菜单，包含缓动、缩放、透明度、呼吸光和分层页面转场
- `MOTION` 页面：BMI270 原始加速度、VQF 四元数派生的 Roll/Pitch/Yaw、动态姿态仪
- `AURA` 页面：低速多层环形/粒子动画，用于展示偏重质量的动画风格
- `SYSTEM` 页面：堆内存、PSRAM 和运行时间
- KEY1（GPIO11）：下一项 / 返回；KEY2（GPIO12）：打开 / 页面动作
- MOTION 页面按 KEY2 可把当前航向设为相对零点

## 硬件映射

| 功能 | GPIO / 地址 |
| --- | --- |
| LCD MOSI / SCLK | GPIO39 / GPIO40 |
| LCD CS / DC / RST / BL | GPIO41 / GPIO45 / GPIO21 / GPIO38 |
| 内部 I2C SDA / SCL | GPIO47 / GPIO48 |
| BMI270 | `0x68`（同时兼容检测 `0x69`） |
| M5PM1 | `0x6E` |
| KEY1 / KEY2 | GPIO11 / GPIO12 |

## 构建与烧录

宿主机项目路径在容器中映射为 `/workspace/esp32_idf/m5_sticks3`：

```bash
docker exec -it -w /workspace/esp32_idf/m5_sticks3 eso-dev bash
source /opt/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`CMakeLists.txt` 通过 `EXTRA_COMPONENT_DIRS` 直接复用相邻参考工程
`esp-idf-icm42688/components/third_party/vqf`。因此两个工程应保持当前同级目录关系。

## 任务与刷新策略

- BMI270 + VQF：100 Hz，固定 10 ms 周期，运行在 CPU1
- LVGL 数据呈现：16 ms 周期（最高约 60 Hz），从线程安全的最新快照读取并做角度平滑
- LVGL tick：5 ms；默认刷新周期：10 ms
- 显存传输：2×60 行 RGB565 DMA 缓冲；LVGL 内存池 96 KiB，阴影缓存 40 px
- MOTION 页面只更新肉眼可见的数值/像素变化，设备静止时不做无效重绘
- UI 过渡：约 430–460 ms 的 ease/overshoot 组合，持续动效使用较慢的往复节奏

实机串口统计中，菜单持续动画约为 75–84 FPS，AURA 多层动画约为 49–51 FPS；
具体数值会随当前页面和脏区面积变化。

## 模块与按键事件

- `create_*_module()` 只负责启动期构造；`AppManager` 使用固定 8 槽注册表，按注册顺序初始化并按反序回滚、停止。
- 按键扫描是独立模块，保持 10 ms 轮询和 30 ms 去抖；稳定的按下、释放边沿通过静态 `ButtonEventBus` 发布。
- 事件总线包含 8 个队列槽和 4 个订阅槽，事件发布、排队与分发过程不申请堆内存。
- UI 订阅按键事件并只在释放边沿执行原有动作；运动数据仍采用最新快照，避免 100 Hz 数据在队列中积压。

## 主机单元测试

模块回滚、停止顺序、按键去抖、订阅与队列溢出可在不连接开发板时验证：

```bash
cmake -S tests -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```
