# StickS3 CMSIS-DAP v2 / SWD 调试器

本工程提供 `USB DAP`、`W-DAP`（Wi-Fi）、`B-DAP`（BLE）三张菜单卡片，
复用 Arm CMSIS-DAP 2.1.2 参考命令引擎，通过 SWD 烧录和调试 STM32 等 Cortex-M 目标。
USB 是 CMSIS-DAP v2 Vendor Bulk 接口。Wi-Fi 使用 OpenOCD 的 CMSIS-DAP TCP 协议；
BLE 使用本工程的加密 GATT 传输，再由电脑桥接成同一个 TCP 协议。

这是 **CMSIS-DAP 调试器**。在 IDE 中选择 CMSIS-DAP / OpenOCD；
不能作为 ST-Link 私有协议设备用于 ST-Link GDB Server 或 STM32CubeProgrammer 的 ST-Link 入口。
芯片识别、Flash 算法和 GDB 服务由电脑端 OpenOCD 及对应的 target 配置提供。

## 接线

按机身 HAT2 标签接线，接线前给目标板断电：

| StickS3 HAT2 | 目标 STM32 | 说明 |
| --- | --- | --- |
| G6（HAT2 pin 6） | SWCLK，常见为 PA14 | 推挽时钟 |
| G7（HAT2 pin 8） | SWDIO，常见为 PA13 | 双向数据 |
| G8（HAT2 pin 9） | NRST | 开漏复位，依赖目标板上拉 |
| GND（HAT2 pin 1） | GND | 必须共地 |

目标板自行供电。这里只支持 **3.3 V 逻辑**，没有 VTref 电压检测或电平转换，
不能直接连接 1.8 V / 5 V 调试信号。不要把 HAT/Grove 的 5 V 接到 SWD 引脚。
短线优先，初次连接从 100–250 kHz 上限开始。G6/G7/G8 避开 WAVE 的 G4/G5、内部 I2C、
LCD、音频和红外。固件不打开 EXT_5V，不给目标板供电。

映射核对自用户提供的 `StickS3_Summary.pdf`、实际名称为
`M5/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf` 的原理图，
以及 [M5Stack 官方 StickS3 PinMap](https://docs.m5stack.com/en/core/StickS3)。
USB 数据线使用板载 GPIO19/20，无需另焊 USB 线。

## 菜单与启停

- 主菜单 KEY1 切换卡片，KEY2 打开。浏览卡片不会启用 DAP。
- 进入页面后，只启动选中的调试传输；三种模式共用一个 SWD 工作任务。
- 页面 KEY1 选择动作，KEY2 执行，长按 KEY2 或选择 Back 返回主菜单。
- `Change limit` 循环设置 100 / 250 / 500 / 1000 / 2000 kHz 上限，默认 1000 kHz。
- `Wiring / stats` 切换接线提示和链路统计。
- `Pause probe` / `Resume probe` 关闭或重新启动当前传输；启动失败可用它重试。
- 无线页面的 `Enable radio` 使用现有通信框架启用对应无线。
  SSID/密码仍在原来的 WIFI 页面设置；原有 Wi-Fi/BLE 开关状态由原框架管理，
  DAP 页面不自动更改它们。退出 DAP 会关闭 TCP 监听/USB DAP，并拒绝 BLE DAP 请求。
- SWD 引脚在 `DAP_Connect` 之前保持输入；DAP_Disconnect、传输断线或退出页面后恢复高阻，
  NRST 不会被保持拉低。停止在工作任务内完成，通常几个 tick，正在进行的低速操作会有短暂延迟。

大字显示本机时钟上限，`SWD cfg` 显示主机请求与上限取小后的配置频率。
这是 GPIO 软件驱动的时钟，指令开销和任务抢占会使实际频率更低；**没有测量并伪报实际 SWCLK**。
主机可请求 1 kHz–2 MHz，超过上限会被限制，低于 1 kHz 返回 DAP_ERROR。
页面还显示 USB 包大小、Wi-Fi 地址/TCP 端口、BLE MTU/加密状态、包计数、错误和最近 ACK。
`SWD ACTIVE` 表示已选择 SWD 端口，目标是否应答要结合 ACK 和 OpenOCD 输出判断。

## USB 烧录和调试 STM32

进入 `USB DAP` 后，板载原生 USB 会从 ESP32 Serial/JTAG 重新枚举为
`StickS3 CMSIS-DAP`。产品名和接口名均带 CMSIS-DAP，接口 0 提供 64 字节
Bulk OUT `0x01` / Bulk IN `0x81`。支持 BOS / Microsoft OS 2.0 WinUSB 描述符，
采用 CMSIS-DAP 标准接口 GUID。序列号来自 ESP32 的 eFuse MAC。

Linux 如遇 USB 权限问题，可安装仓库中的 `tools/99-sticks3-dap.rules` 后重新插拔。
开发 USB 标识为 `303a:4004`，来自 Espressif 开发用 VID；量产产品应使用自己的分配标识。
Windows/WSL 需要把**重新枚举后的设备**重新挂入 WSL，原 Serial/JTAG 的挂载不代表 DAP 也已挂载。

在工程目录，用正确的 STM32 系列 target 配置替换下例的 `stm32f1x.cfg`：

```bash
# 读取/连接检查，不擦写 Flash。
openocd -f tools/openocd/sticks3-usb.cfg -f target/stm32f1x.cfg \
  -c "init; reset halt; targets; shutdown"

# ELF 自带地址。只有这条命令才烧录目标板。
openocd -f tools/openocd/sticks3-usb.cfg -f target/stm32f1x.cfg \
  -c "program /path/to/firmware.elf verify reset exit"

# BIN 必须给正确的目标地址。
openocd -f tools/openocd/sticks3-usb.cfg -f target/stm32f1x.cfg \
  -c "program /path/to/firmware.bin 0x08000000 verify reset exit"

# 保持 OpenOCD 运行，GDB 默认连接本机 TCP 3333。
openocd -f tools/openocd/sticks3-usb.cfg -f target/stm32f1x.cfg
arm-none-eabi-gdb /path/to/firmware.elf
```

GDB 会话：

```gdb
target extended-remote localhost:3333
monitor reset halt
load
break main
continue
```

STM32F4 通常用 `target/stm32f4x.cfg`，H7 通常用 `target/stm32h7x.cfg`，
应按实际 MCU 选择。需要连接复位下的目标时，接上 NRST，并在 OpenOCD 初始化前追加
`-c "reset_config srst_only srst_nogate connect_assert_srst"`。
未接 NRST 时可在 target 配置后追加 `-c "reset_config none"`，由目标支持情况决定软件复位行为。
SWO、JTAG、串口桥、离线烧录及 STM32 安全认证流程不在本次实现范围内。

## Wi-Fi

先在原 WIFI 菜单配置 STA，或者让电脑连接设备受密码保护的热点，然后打开 `W-DAP`。
屏幕优先显示 STA 地址，没有 STA 时显示热点地址（默认 `192.168.4.1`）。
服务监听所有本机 IPv4 接口的 TCP `4441`，只允许一个调试客户端占用 SWD。
页面未打开/已暂停时不会监听此端口。

```bash
# 仅读取 DAP 身份，不接触目标 SWD。
python3 tools/dap_bridge.py info --host 192.168.4.1

openocd -c "set DAP_HOST 192.168.4.1" \
  -f tools/openocd/sticks3-wifi.cfg -f target/stm32f1x.cfg
```

电脑端 OpenOCD 必须支持 `cmsis-dap backend tcp`。本次验证的容器版本
`v0.12.0-esp32-20251215` 已支持；部分发行版的旧版 0.12.0 尚不支持，可用
`openocd -c "adapter driver cmsis-dap; help cmsis-dap; shutdown"` 检查。
Wi-Fi 的 `min_timeout` 默认 1000 ms，适合无线延迟。
TCP DAP 协议没有 TLS 或应用密码，限可信局域网/设备密码热点使用，不转发到公网。

## BLE

ESP32-S3 使用 BLE GATT，不是经典蓝牙 SPP。电脑需要蓝牙适配器、Python 3.11+ 和 Bleak：

```bash
python3 -m venv .venv-dap
. .venv-dap/bin/activate
pip install -r tools/dap-requirements.txt
python tools/dap_bridge.py scan
# StickS3 上进入 B-DAP，并确保 BLE 已开启。
python tools/dap_bridge.py ble --address AA:BB:CC:DD:EE:FF
```

桥接器首次连接时请求系统配对；沿用现有 NimBLE Secure Connections、128 位链路加密及 NVS 绑定。
无桌面配对代理的 Linux 主机如首次配对超时，先在终端完成一次系统配对，再运行桥接器：

```text
bluetoothctl --agent NoInputNoOutput
pair AA:BB:CC:DD:EE:FF
# 等待 Pairing successful 后退出 bluetoothctl。
quit
```

该代理沿用设备的 Just Works 配对方式。不要在配对完成前退出代理。
Linux 桥接器固定按最小 ATT MTU 23 分片，避免依赖 BlueZ 不公开的 MTU 查询接口。

等待桥接器显示 `Ready`（BLE 配对已完成）后，在另一个终端执行：

```bash
python tools/dap_bridge.py info --host 127.0.0.1
openocd -f tools/openocd/sticks3-ble.cfg -f target/stm32f1x.cfg
```

macOS 的 `--address` 使用扫描显示的设备 UUID。若操作系统缓存了旧固件的 GATT 服务列表，
需在系统蓝牙设置中删除旧设备再配对。WSL/Docker 不一定能访问宿主机蓝牙，
可直接在 Windows/Linux/macOS 主机运行桥接器和 OpenOCD。
桥接器只监听 `127.0.0.1:4441`，一个 BLE 连接上同时只允许一个 OpenOCD 会话。
正常关闭 OpenOCD 会释放 SWD，并保持 BLE 就绪；紧接着到来的新会话会等待上一次释放完成。
另一个仍在调试的客户端会被拒绝。链路/命令失败会退出桥接器，需重新启动。

BLE 支持默认 MTU 23，不要求协商大 MTU；大包自动分片。它的时延和吞吐不及 USB/Wi-Fi，
更适合近距离临时调试。读写超时会断开整条会话，**不自动重放 Flash 写入命令**。
退出 `B-DAP` 会拒绝后续读写并释放 SWD；原有 JSON BLE 服务继续由原通信模块管理。

## 协议和实现

- `components/debug_probe`：单独 CPU0 工作任务，菜单只发非阻塞模式/速率请求。
- `vendor/`：Arm Apache-2.0 源码，固定提交及修改记录见其中 README 和 LICENSE。
- 64 字节 DAP 包，声明一个在途包；不声明原子命令/JTAG/SWO/UART 能力。
- 支持 DP/AP 读写、AP posted read、TransferBlock、match mask/value、WAIT 重试、FAULT、
  奇偶校验、SWJ/SWD sequence、HostStatus、SWJ pins/clock、复位和无响应 TransferAbort。
- 包解析先检查输入与最坏响应长度，再进入参考引擎；未知/不支持/畸形命令返回 `0xff`。
- 一条传输中的 WAIT/match 操作有 200 ms 工作预算，SWJ_Pins 等待限制为 100 ms。
  低频单个 SWD 周期序列本身可能增加停止延迟。
- TCP 的 8 字节小端头：`44 41 50 00 | payload_len:u16 | type:u8 | reserved:0`，
  请求 type=1，响应 type=2。处理分段/合包、部分发送、2 秒残帧超时与 TCP keepalive。
- BLE Service `8d5d0001-673a-4c8e-a791-005041443373`；RX 为 `...0002...`，TX 为 `...0003...`。
  GATT 定义随现有 host 注册，进入 DAP 页面前所有 DAP 访问都被拒绝。
- RX 必须 encrypted Write Request：`sequence:u16 | offset:u8 | total:u8 | payload`。
  默认 MTU 每次带 16 字节 DAP 数据，拒绝错序、越界、跨客户端或未完成请求上的覆盖。
- TX 必须 encrypted Read/Read Blob：`sequence:u16 | ready:u8 | length:u16 | payload`。
  busy 返回 ready=0/length=0；完成后保留完整结果，可多次读取，直到下个请求。
  固定事务编号阻止断开重连后连接句柄复用造成旧响应污染。

USB 恢复：退出/暂停 `USB DAP` 会卸载 TinyUSB，并将内部 PHY 重新绑定到 Serial/JTAG。
USB DAP 活动期间原串口日志和自动下载口不可用。正常退出后用恢复的 ttyACM 重新烧录；
如果运行态 USB 不可用，按板子的 BOOT/复位操作进入 ROM 下载模式后重新烧录。
固件不开机自动进入 DAP，也不修改 USB eFuse。

## 构建与验证

容器内实际环境为 ESP-IDF 5.5.4；依赖固定 TinyUSB wrapper 2.2.1，锁文件记录 TinyUSB 0.21.0~2。
LVGL 固定为原锁文件的 9.5.0，避免加入 USB 依赖时升级无关界面组件。

```bash
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp-idf/export.sh >/dev/null && idf.py -B build/codex-dap build'

# 以下才会更新 StickS3 本机固件。
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp-idf/export.sh >/dev/null && idf.py -B build/codex-dap -p /dev/ttyACM0 flash'

# 完整无硬件检查，使用独立 SDKCONFIG。
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp-idf/export.sh >/dev/null && bash tools/verify_debug_probe.sh'
```

2026-09-22 验证环境：用户连接的 StickS3、Linux 主机、Docker ESP-IDF 5.5.4，
未连接 STM32 目标板。原 8 MB Flash 已备份至
`build/dap-hardware/original-flash.bin`，包含原 NVS 配置，应私下保存。

| 检查 | 结果 |
| --- | --- |
| Docker 完整正式固件构建 | 通过，约 1.71 MiB，3 MiB 应用分区约 43% 空余 |
| 原有主机回归 + 协议/SWD 引擎/BLE 桥接（ASan/UBSan） | 9 个 CTest 套件通过 |
| 原有外设页面 + DAP 页面生命周期/文字边界 | 2 个 CTest 套件通过；LVGL 渲染预览已检查 |
| USB 实际枚举 | Linux 识别 `303a:4004`；接口、Bulk 端点、BOS、WinUSB 描述符检查通过 |
| USB DAP 命令及退出恢复 | 标准命令、畸形包、TransferAbort、200 次连续收发通过；两轮连续启停均恢复 Serial/JTAG、ttyACM 和 GPIO 输入状态 |
| Wi-Fi 真链路 | 设备热点模式下，200 次连续收发、TCP 分帧、单客户端独占、残帧超时、错误头断开、重新连接通过 |
| BLE 真链路 | 系统配对、加密（设备报告 MTU 259）、16 字节请求分片、58 字节响应、200 次连续收发通过 |
| OpenOCD 实际连接 | USB、Wi-Fi、BLE 均识别设备并完成 SWD 接口初始化；BLE 连续会话交接通过 |
| 菜单与关闭状态 | 主菜单不监听 TCP、拒绝 BLE DAP；速率选择、接线视图、暂停/恢复通过；退出/客户端断开后 G6/G7/G8 输出使能位均为 0 |
| 正式固件烧录及重启 | 写入校验通过；主菜单正常运行，约 72–74 fps；原生串口可用，测试控制入口不存在 |
| STM32 Flash、断点、单步 | 未接目标板，尚未验证 |
| 电气时序与其他平台 | 实际 SWCLK/NRST 波形、Windows WinUSB 绑定、Wi-Fi STA 和 ATT MTU 23 的 Read Blob 真链路尚未验证 |

设备最终保留的是 `build/codex-dap/m5_sticks3.bin` 正式镜像，保留原有 NVS 配置，
主机 Wi-Fi 已恢复，临时热点配置已删除。镜像 SHA-256：
`e3acdb5f81e4c14be82a89b7571e21ece75c0a4d3b359ae535a9910326c4bee3`。

实机测试修正了 BLE 会话清理期间立即重连的竞争，以及低速页面恢复时的配置频率显示。
200 次 DAP_Info 往返在本次环境约为 USB 0.4 s、热点 Wi-Fi 11.3 s、BLE 42.9 s；
这些是短命令往返耗时，**不是 STM32 烧录速度或 SWCLK 测量结果**。

自动页面控制仅存在于显式启用 `M5_STICKS3_DAP_SMOKE_TEST` 的测试构建：

```bash
cp sdkconfig build/dap-hardware.sdkconfig
idf.py -B build/dap-hardware-idf \
  -D SDKCONFIG="$PWD/build/dap-hardware.sdkconfig" \
  -D M5_STICKS3_DAP_SMOKE_TEST=ON build
```

该构建仍从主菜单启动，USB Serial/JTAG 接受换行结尾的
`dap usb`、`dap wifi`、`dap ble`、`dap off`、`dap status`，
以及调用实际页面按钮动作的 `dap next`、`dap select`、`dap back`。
USB 页面 60 秒后、无线页面 180 秒后自动返回，避免测试中丢失控制入口。
普通构建中这些串口命令和自动返回均不存在；不要将测试构建用作日常烧录器。

接上目标板后的下一步是 USB 低速连接 → ELF 烧录校验/断点 → Wi-Fi → BLE，
并用示波器核对 SWCLK 频率及 NRST 开漏释放。

参考：[Arm USB 接口规范](https://arm-software.github.io/CMSIS_5/DAP/html/group__DAP__ConfigUSB__gr.html)、
[CMSIS-DAP 命令](https://arm-software.github.io/CMSIS-DAP/latest/group__DAP__Commands__gr.html)、
[OpenOCD CMSIS-DAP TCP 配置](https://openocd.org/doc/html/Debug-Adapter-Configuration.html)、
[Espressif USB Device Stack](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html)、
[Bleak Client API](https://bleak.readthedocs.io/en/latest/api/client.html)。
