# CANopen / CAN-FD framework for ESP32-C5

这是一个面向产品化的 ESP-IDF CANopen 框架起点，而不是把协议逻辑、驱动和业务代码揉在一起的演示程序。协议核心不依赖 ESP-IDF，ESP32-C5 只通过传输适配层接入；实时路径使用静态队列和固定容量存储，协议任务单线程拥有状态，便于测试、移植和做最坏执行时间分析。

当前阶段完成了用户要求的“基本 CANopen + CAN-FD”闭环，并已经在真实 ESP32-C5 与 `1209:2323` candleLight USB-CANFD 上验证。它还不是对全部 CiA 301/1301 服务的合规性声明；尚未实现的产品级能力在下面明确列出，后续可以在现有边界内增量补齐。

## 已实现

- ESP-IDF 5.5.4 的新 TWAI on-chip 驱动，目标为 ESP32-C5。
- 精确位时序：仲裁段 1 Mbit/s、采样点 80%、SJW 5；数据段 5 Mbit/s、采样点 75%、SJW 3；CAN-FD 与 BRS 开启。
- NMT 从站：Start、Stop、Pre-operational、Reset node、Reset communication，支持节点号和广播寻址。
- Boot-up 与 Heartbeat producer；默认节点 `0x21`，默认每 `100 ms` 发送 `0x721` 心跳。
- Node-ID 可通过 `0x2001:01` 修改，并以标准 `0x1010:01 = "save"` 命令原子提交到 ESP32 NVS；重启后生效。
- SDO server：expedited 和 segmented upload/download，完整 abort 响应与传输超时；固定 512 字节工作区。
- 4 组 RPDO + 4 组 TPDO，运行时通信参数和映射参数，最多 16 个映射项、64 字节；超过 8 字节自动使用 FD+BRS。
- SYNC consumer、事件定时 TPDO、同步 TPDO、inhibit time，以及 EMCY producer API。
- 固定容量对象字典、读写权限、PDO 映射权限、读写 hook；初始化后冻结结构。
- ISR 只做驱动取帧、静态队列投递和原子计数；协议状态只在服务任务内更新。
- 固定 TX slot 保存异步发送所需的 `twai_frame_t` 与 payload，避免栈上帧生命周期错误。
- Bus-off 异步请求、任务上下文恢复，以及 RX drop/TX fail/bus error/recovery 统计。
- 固定容量 APP module manager，顺序初始化、失败回滚、逆序释放。
- Linux 主机单元测试，以及绕过 SocketCAN 内核模块的 gs_usb 真实硬件验收工具。

## 仓库结构

```text
.
├── main/                         # 产品装配与 Kconfig
├── components/
│   ├── app/                     # 模块生命周期与任务基类
│   ├── can_transport/           # 平台无关 CAN/CAN-FD 帧和 ITransport
│   ├── canopen/                 # 平台无关 CANopen 协议核心
│   └── canopen_esp32/           # ESP32-C5 TWAI-FD 与 FreeRTOS 适配
├── tests/                        # 不需要 ESP-IDF 的协议单元测试
├── tools/gsusb_smoke.c           # candleLight 硬件回归测试
├── eds/esp32c5_canopen_fd.eds    # 当前对象字典的 EDS
└── docs/architecture.md          # 架构、并发和扩展约束
```

## 硬件与默认配置

ESP32-C5 的 TWAI 引脚是数字逻辑电平，必须连接支持 CAN-FD 及 5 Mbit/s 数据段的外部收发器；总线两端需要正确的 120 Ω 终端。默认配置如下：

| 项目 | 默认值 |
|---|---:|
| Target | ESP32-C5 |
| TX / RX | GPIO4 / GPIO5 |
| Node ID | `0x21` |
| Heartbeat | 100 ms |
| Nominal bitrate | 1 Mbit/s, SP 80%, SJW 5 |
| Data bitrate | 5 Mbit/s, SP 75%, SJW 3 |
| SDO request / response | `0x621` / `0x5A1` |

等价的 SocketCAN 参数是：

```sh
sudo ip link set can0 type can bitrate 1000000 sample-point 0.8 \
  dbitrate 5000000 dsample-point 0.75 sjw 5 dsjw 3 fd on
sudo ip link set can0 up
```

初始节点号、心跳、引脚、队列深度、任务优先级和 `0x1018` identity 均可在 `./tools/idf.sh menuconfig` 的 `CANopen ESP32-C5` 菜单修改；一旦通过 `0x1010` 保存过 Node-ID，NVS 中的值会优先于 menuconfig 默认值。

## 修改 Node-ID 并断电保存

该流程与 `hex-motor-gui` 的 `change_node_id` 完全兼容。假设当前节点为 `0x21`，要改成 `0x22`：

1. 通过旧节点的 SDO 写 `0x2001:01 = 0x22`（`UNSIGNED8`）。
2. 仍通过旧节点写 `0x1010:01 = 0x65766173`（`UNSIGNED32`，线上字节为 ASCII `s a v e`）。
3. 收到成功响应后断电重上电或执行 CPU 复位；节点将从 NVS 恢复 `0x22`。

对应的经典 CAN SDO 请求 payload 为：

```text
0x621  2F 01 20 01 22 00 00 00
0x621  23 10 10 01 73 61 76 65
```

新 ID 只允许 `1..127`；非法值返回 SDO abort `0x06090030`，签名错误或 NVS 提交失败返回 `0x08000020`。在真正重启前，Heartbeat、SDO 和 PDO 继续使用旧 ID，避免保存响应发到突然变化的 COB-ID。ESP32 端使用 NVS namespace `canopen`、key `node_id`，重复保存同一值不会产生额外 flash 擦写。

## 构建、烧录与监视

当前 WSL 工程在 `esp-dev` 中挂载为 `/workspace/canopen-esp32c5`：

```sh
docker exec -it esp-dev bash
cd /workspace/canopen-esp32c5
./tools/idf.sh set-target esp32c5
./tools/idf.sh build
./tools/idf.sh -p /dev/ttyACM0 flash monitor
```

正常启动会先发送一次 Boot-up `0x721 00`，随后进入 Pre-operational 并按 100 ms 发送 `0x721 7F`。如果总线上只有本节点、没有其他 active 节点 ACK，日志中的 `fail` 和 `err` 持续增长是 CAN 物理层的预期行为，不是心跳调度停止。

## 主机协议测试

主机测试使用 fake transport，不依赖 ESP-IDF 或硬件：

```sh
cmake -S tests -B /tmp/canopen-esp32c5-tests
cmake --build /tmp/canopen-esp32c5-tests -j
ctest --test-dir /tmp/canopen-esp32c5-tests --output-on-failure
```

覆盖 NMT/Boot-up/Heartbeat、expedited 与 segmented SDO、Node-ID 校验/保存/失败回传，以及 12 字节 FD RPDO/TPDO 映射。

## USB-CANFD 硬件回归

`tools/gsusb_smoke.c` 使用与 `hex-motor-gui` 的 `can-transport::gs_usb` 相同的 vendor protocol 和位时序，因此不要求 WSL 内核提供 `gs_usb.ko`。Linux 环境需要 libusb 开发包和对 USB 设备节点的权限：

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tools/gsusb_smoke.c \
  -lusb-1.0 -o /tmp/gsusb_smoke
sudo /tmp/gsusb_smoke --node-id 0x21
```

只执行 Node-ID 写入与 NVS 保存（不立即切换当前 COB-ID）：

```sh
sudo /tmp/gsusb_smoke --node-id 0x21 --set-node-id 0x22
```

该程序依次验收：

1. `0x721/0x7F` Pre-operational 心跳；
2. `0x1018:1` expedited SDO upload；
3. 通过 SDO 配置 TPDO1 为 3 个 32-bit 映射项；
4. NMT Start 后接收 12 字节、FD+BRS 的 TPDO1；
5. Reset communication 后确认 `00 -> 7F` 心跳序列并恢复默认通信配置。

如果适配器在 Docker 容器启动之后才接入，特权容器也可能看不到新增的 `/dev/bus/usb` 节点。推荐在 USB attach 完成后重启容器，或启动容器时显式映射 `/dev/bus/usb`。

## 当前协议边界

| 能力 | 状态 |
|---|---|
| NMT slave / boot-up / heartbeat producer | 已实现并上板验证 |
| SDO expedited + segmented server | 已实现并测试 |
| Dynamic PDO / SYNC / event timer / FD+BRS PDO | 已实现并上板验证 |
| EMCY producer API | 已实现，尚需产品错误管理器接入 |
| Node-ID 修改与 NVS 断电保存 | 已实现；主机测试与固件构建通过，断电闭环待复验 |
| Heartbeat consumer supervision | OD 已预留，超时状态机待实现 |
| SDO block transfer | 待实现 |
| TIME、LSS、其他通信与应用参数持久化、DCF 下载 | 待实现 |
| CiA 301/1301 一致性测试与认证 | 待接入正式 conformance test |

这里采用与现有 `hex-motor-gui` 产品链一致的策略：NMT、Heartbeat、SDO 等控制面保持经典 CAN 帧，超过 8 字节的 PDO 使用 CAN-FD+BRS。这是明确的产品协议扩展，不能仅凭实现结果宣称完整符合 CiA 1301。

详细设计和扩展规则见 [docs/architecture.md](docs/architecture.md)，对象字典见 [docs/object-dictionary.md](docs/object-dictionary.md)。
