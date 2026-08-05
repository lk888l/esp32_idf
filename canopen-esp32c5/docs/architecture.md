# Architecture

## Design goals

这个工程优先保证边界清楚、确定性内存、可测试性和故障可观测性。ESP32-C5 不是协议实现的中心：它只是当前的 CAN-FD transport 和任务调度平台。核心协议可以被 Linux、另一款 MCU 或测试 fake transport 复用。

```text
main / product composition
        |
        v
app::Manager ---- lifecycle / rollback ---- app::Module
        |
        v
canopen_esp32::CanopenModule
        |                         \
        v                          v
canopen::StandardProfile      EspTwaiTransport
  Node / SDO / PDO / OD       ISR / queues / slots
        |                          |
        +------ can::ITransport ---+
```

依赖方向始终向下：`canopen` 只能依赖 `can_transport`，不能包含 ESP-IDF 或 FreeRTOS 头文件。平台适配层依赖协议核心和 ESP-IDF。产品层只负责组装配置和模块。

## Components

### `can_transport`

定义统一的 `can::Frame` 和最小 `can::ITransport::send()` 边界。帧结构同时表达 standard/extended、data/RTR、classic/FD 和 BRS，不把 ESP-IDF 的 `twai_frame_t` 泄漏到协议层。

新平台至少需要实现可靠的 send 语义，并把接收帧送给 `canopen::StandardProfile::handle()`。当前 ESP 适配器额外提供 receive queue、统计和 maintenance；若以后需要多总线或统一诊断，可把这些能力拆成可选接口，而不改变协议层。

### `canopen`

- `ObjectDictionary`：固定 224 项，按 index/subindex 有序查找，显式 access/type/PDO-mappable 元数据；初始化完成后 freeze。
- `SdoServer`：每节点一个状态机和固定 512 字节 segmented transfer buffer；超时或协议错误发送标准 abort code。
- `PdoManager`：4 RPDO + 4 TPDO，每个最多 16 个映射；所有映射写入先验证长度、权限和合法配置顺序。
- `Node`：唯一 NMT 状态所有者，调度 boot-up、heartbeat、SYNC、SDO、PDO 和 EMCY。
- `StandardProfile`：注册 CiA 301 通信对象、identity、`0x1010` 保存入口和厂商 Node-ID 对象；`ParameterStorage` 是无平台依赖的持久化提交边界。

核心代码没有运行期容器扩容、异常、RTTI、互斥锁或平台调用。字节序在 OD/SDO 边界显式转换为 CANopen little-endian。

### `canopen_esp32`

`EspTwaiTransport` 使用 ESP-IDF 5.5 的新 event-driven TWAI API。关键约束：

`EspNvsParameterStorage` 在协议对象构造前初始化 NVS 并恢复 Node-ID；只有 `nvs_commit()` 成功后才向 SDO 层报告保存成功。namespace 固定为 `canopen`，key 为 `node_id`。

- RX ISR 从驱动取帧后复制到静态 FreeRTOS queue，不在 ISR 执行协议逻辑。
- 异步 TX 不能引用调用者栈内存；固定 TX slot 同时保存 native descriptor 和 64 字节 payload，直到 `on_tx_done` 才回收。
- 所有统计使用 relaxed atomics；ISR 只做有界工作。
- Bus-off callback 只置位，实际 `twai_node_recover()` 在任务上下文执行。
- FD payload 自动向合法 DLC 长度 `8/12/16/20/24/32/48/64` 补零，但协议帧的逻辑长度仍由映射决定。

### `app`

`app::Manager` 是固定 8 槽模块注册表。初始化按注册顺序执行；任一模块失败时按相反顺序回滚。正常释放同样逆序进行。模块对象只在启动装配阶段由 `unique_ptr` 分配，实时路径不分配。

## Concurrency ownership

协议服务任务是 NMT 状态、SDO transfer、PDO runtime mapping 和对象字典写操作的唯一执行者。主循环只调用模块的低频 `process()` 输出统计；ISR 通过队列和原子变量通信。

Node-ID 保存是极低频维护操作：NVS commit 在协议任务中同步完成，以保证成功的 SDO response 表示数据已经持久化。提交期间可能产生一次 flash 写入延迟，不应在运动控制实时窗口内频繁调用。

这条所有权规则必须保持：业务任务不能直接写 OD backing storage。业务集成应使用消息队列、双缓冲快照，或者在协议任务内执行的 read/write hook。对于多字节且可能被异步业务更新的对象，必须提供一致性快照，不能依赖 ESP32 对 64-bit 值的非原子访问。

## Bit timing

ESP32-C5 TWAI 默认控制器时钟为 80 MHz。工程使用 advanced timing 覆盖自动求解结果：

| Phase | BRP | Prop | TSEG1 | TSEG2 | SJW | Result |
|---|---:|---:|---:|---:|---:|---|
| Nominal | 1 | 31 | 32 | 16 | 5 | 1 Mbit/s, SP 80% |
| Data | 1 | 5 | 6 | 4 | 3 | 5 Mbit/s, SP 75% |

计算式为 `bitrate = 80 MHz / (BRP * (1 + Prop + TSEG1 + TSEG2))`。这些值与 `hex-motor-gui` 的 `GsUsbConfig::fd_1m_5m()` 完全一致。

## CANopen over CAN-FD policy

控制面使用经典 CAN：NMT、SYNC、EMCY、Heartbeat 和 SDO 都拒绝 FD 形式或固定为经典 8 字节帧。PDO 映射总长度不超过 8 字节时发送 classic；9–64 字节时发送 FD+BRS。这既保持传统工具对控制服务的可见性，也兼容现有 hex-motor 产品的长 PDO。

这套策略是产品 profile，不应与完整 CiA 1301 合规性混淆。未来若选择正式 CiA 1301，需要单独冻结协议版本，按规范审查 USDO、PDO/SDO 行为和 EDS/XDD 表达，并运行一致性测试。

## Failure model

- RX queue 满：丢弃新帧并增加 `rx_dropped`，协议任务不中断。
- TX slot 用尽：调用方得到 queue-full/timeout，不覆盖未完成帧。
- TX 未被 ACK 或驱动失败：增加 `tx_failed`；周期服务继续运行。
- Bus-off：ISR 请求恢复，服务任务调用驱动恢复并计数。
- SDO 非法访问：返回精确 abort code，不部分写入对象。
- NVS 初始化或 namespace 打开失败：CANopen 模块拒绝启动，避免设备表现为支持保存但实际不持久。
- Node-ID 越界：`0x2001:01` 返回 `0x06090030`；保存签名错误或 NVS commit 失败：`0x1010:01` 返回 `0x08000020`。
- 掉电一致性：未写 `save` 的 RAM 值不会生效；成功 commit 后重启从 NVS 恢复。重复保存相同值不写 flash。
- 模块初始化失败：Manager 逆序回滚，不进入半启动 running 状态。

## Extension rules

1. 新业务对象应由产品 profile 注册，不要把厂商对象硬编码进 SDO/PDO。
2. 所有可映射对象必须显式声明 `pdo_mappable`，RPDO 还必须可写。
3. 新 transport 不得让协议层依赖平台句柄。
4. ISR 路径禁止动态分配、日志格式化、阻塞和协议状态变更。
5. 增加协议服务时先写 host test，再接入 ESP task。
6. 修改默认 OD 时同步更新 EDS、对象字典文档和硬件 smoke test。

## Next product milestones

建议按风险顺序继续：heartbeat consumer 与 error manager；扩展持久化参数注册表和受控恢复默认值；SDO block；LSS；正式 product profile/EDS 或 XDD 生成链；总线负载与长时间 bus-off/queue saturation 测试；最后接入 CiA conformance test。
