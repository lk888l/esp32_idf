# M5-StickS3 Motion UI + Pocket Arcade

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
- `SYSTEM` 页面：Wi-Fi IP、BLE 状态、堆内存、PSRAM 和运行时间
- `ARCADE` 页面：三款针对 135×240 小屏和双按键设计的小游戏
- `TILT QUEST`：倾斜设备控制小球绕过障碍，45 秒内收集 5 个信标
- `METEOR DODGE`：倾斜左右闪避陨石，KEY2 启动带冷却时间的护盾
- `TAP RUNNER`：不依赖 IMU，按 KEY2 跳过随机高度的障碍
- 受保护 Wi-Fi 热点、STA 配网与重试，提供统一 HTTP JSON API
- BLE 加密 GATT、持久配对、长响应读取和通知，与 HTTP 共用查询、回显、遥测和配网协议
- KEY1（GPIO11）：下一项 / 返回；KEY2（GPIO12）：打开 / 页面动作
- MOTION 页面按 KEY2 可把当前航向设为相对零点

游戏规则位于独立的 `mini_games` 组件，不依赖 LVGL 或 ESP-IDF，主机测试可直接
覆盖物理边界、计时、护盾冷却和跳跃周期。需要姿态的游戏按需启动 BMI270，离开
游戏后释放传感器；纯按键游戏不会额外开启 IMU。

## 小游戏操作

| 位置 | KEY1 | KEY2 | 长按 KEY2 |
| --- | --- | --- | --- |
| 主菜单 | 下一项 | 打开 | - |
| ARCADE 选择页 | 下一款游戏 | 开始 | 返回主菜单 |
| 游戏内 | 返回 ARCADE | 校准 / 护盾 / 跳跃 | 重新开始 |

`TILT QUEST` 和 `METEOR DODGE` 进入时会把当前手持角度作为中立姿态。建议先自然
握平设备，再开始游戏；`TILT QUEST` 中可随时短按 KEY2 重新校准。

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

当前开发容器名为 `esp-dev`，宿主机项目路径在容器中映射为
`/workspace/esp32_idf/m5_sticks3`：

```bash
docker exec -it -w /workspace/esp32_idf/m5_sticks3 esp-dev bash
source /opt/esp/idf/export.sh
idf.py -B build/codex-idf build
idf.py -B build/codex-idf -p /dev/ttyACM0 flash monitor
```

Windows 上的 ESP32-S3 原生 USB Serial/JTAG 需要先挂入 WSL。BUSID 以
`usbipd list` 的实际输出为准：

```powershell
usbipd list
usbipd attach --wsl --busid <BUSID>
```

WSL 中出现 `/dev/ttyACM0` 后，重启以 `--privileged` 运行的开发容器即可让设备
节点进入容器；也可以在容器中按 WSL 端显示的主、次设备号创建同名节点。

`CMakeLists.txt` 通过 `EXTRA_COMPONENT_DIRS` 直接复用相邻参考工程
`esp-idf-icm42688/components/third_party/vqf`。因此两个工程应保持当前同级目录关系。

## 任务与刷新策略

- BMI270 + VQF：100 Hz，固定 10 ms 周期，运行在 CPU1
- LVGL 数据与游戏呈现：16 ms 周期（最高约 60 Hz），从线程安全的最新快照读取
- LVGL tick：5 ms；默认刷新周期：10 ms
- 显存传输：2×60 行 RGB565 DMA 缓冲；LVGL 内存池 96 KiB，阴影缓存 40 px
- MOTION 页面只更新肉眼可见的数值/像素变化，设备静止时不做无效重绘
- UI 过渡：约 430–460 ms 的 ease/overshoot 组合，持续动效使用较慢的往复节奏

本次实机冒烟测试中，三款游戏页面约为 47–55 FPS，返回菜单后约为 68–70 FPS；
具体数值会随当前页面、动画阶段和脏区面积变化。

## 模块与按键事件

- `create_*_module()` 只负责启动期构造；`AppManager` 使用固定 8 槽注册表，按注册顺序初始化并按反序回滚、停止。
- 按键扫描是独立模块，保持 10 ms 轮询和 30 ms 去抖；稳定的按下、释放边沿通过静态 `ButtonEventBus` 发布。
- 事件总线包含 8 个队列槽和 4 个订阅槽，事件发布、排队与分发过程不申请堆内存。
- UI 订阅按键事件；常规页面在释放边沿执行动作，游戏内 KEY2 在按下边沿立即响应，
  以降低跳跃和护盾延迟。运动数据仍采用最新快照，避免 100 Hz 数据在队列中积压。

## 主机单元测试

模块回滚、停止顺序、按键去抖、事件队列以及小游戏规则可在不连接开发板时验证：

```bash
cmake -S tests -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

## 实机冒烟测试

默认关闭的 `M5_STICKS3_HW_SMOKE_TEST` 会自动依次运行三款游戏，并验证 BMI270 的
启动、100 Hz 采样和释放流程；结束后自动回到主菜单。请使用独立构建目录，避免
测试开关污染正式固件缓存：

```bash
idf.py -B build/hw-smoke -D M5_STICKS3_HW_SMOKE_TEST=ON build
idf.py -B build/hw-smoke -p /dev/ttyACM0 flash monitor
```

验证完成后重新烧录 `build/codex-idf` 中的正式固件。

## Wi-Fi / BLE 通信框架

新增 Wi-Fi STA + 独立密码备用热点、NimBLE GATT 加密收发、公共 API v1、NVS 配置与有界异步命令队列。SYSTEM 页面可查看 IP 和 BLE 状态。

完整架构、配网步骤、接口协议、客户端用法与验证方式见 [通信框架文档](docs/connectivity.md)。
