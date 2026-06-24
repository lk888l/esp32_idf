# Hexfellow Motor Controller — PDO 映射与架构文档

## 1. 系统概述

`HexfellowMotorController` 是基于 CANopen over CAN-FD 的多电机控制器，支持**多个电机**挂载在同一条 CAN 总线上。控制架构采用一对多（one-to-many）的 RPDO 广播方式，主站以固定 1ms 周期向所有电机发送控制指令，电机通过各自的 TPDO 异步回报状态。

| 参数 | 值 | 说明 |
|------|-----|------|
| 主站 Node-ID | `0x10` | 电机监控此节点的心跳 |
| 共享 RPDO COB-ID | `0x190` | 一对多控制帧 |
| 默认最大电机数 | `8` | CAN-FD 64 字节帧：MIT 8 字节/电机，Velocity 6 字节/电机 |
| 控制周期 | `1 ms` | RPDO 发送周期 |
| 主站心跳周期 | `50 ms` | 电机心跳超时 `250 ms` |
| 固件版本要求 | `v8 ~ v9` | 工厂 UID: `0x4859444C` |

以上部分参数可在对应hpp文件中修改

---

## 2. CANopen COB-ID 分配

| 名称 | COB-ID | 方向 | 说明 |
|------|--------|------|------|
| NMT | `0x000` | 主→从 | 网络管理命令 |
| TPDO1 | `0x180 + Node-ID` | 从→主 | 高频反馈数据 |
| TPDO2 | `0x280 + Node-ID` | 从→主 | 状态/诊断数据 |
| RPDO1 | `0x190`  | 主→从 | 一对多控制指令 |
| RxSDO | `0x600 + Node-ID` | 主→从 | SDO 下载 |
| TxSDO | `0x580 + Node-ID` | 从→主 | SDO 上传 |
| Heartbeat | `0x700 + Node-ID` | 从→主 | 节点心跳 |


---

## 3. 工作模式

系统支持两种 CiA402 工作模式（默认为MIT模式）：

| 模式 | 枚举值 | CiA402 值 (`0x6060`) | 说明 |
|------|--------|---------------------|------|
| MIT (扭矩/位置/速度/阻抗) | `HEXFELLOW_MODE_KIND_MIT = 0` | `5` | 5 维控制：位置 + 速度 + 扭矩 + Kp + Kd |
| Profile Velocity | `HEXFELLOW_MODE_KIND_VELOCITY = 1` | `3` | 速度控制 + 扭矩限幅 |

不同电机可配置为不同模式，独立运行。

---

## 4. TPDO 映射（电机 → 主站）

### 4.1 TPDO1 — 高频反馈帧（12 字节）

**COB-ID:** `0x180 + Node-ID`  
**发送参数:** 传输类型 255 (异步/事件驱动), 抑制时间 0.5ms, 事件定时器 1ms

| 子索引 | 对象字典 | 长度 | 位宽 | 数据类型 | 描述 |
|--------|----------|------|------|----------|------|
| 1 | `0x6064:00` | 4 字节 | 32 | REAL32 | 实际位置 (rev) |
| 2 | `0x1013:00` | 4 字节 | 32 | UINT32 | 高速时间戳 (μs) |
| 3 | `0x6077:00` | 2 字节 | 16 | INT16 | 实际扭矩 (‰ 额定扭矩) |
| 4 | `0x603F:00` | 2 字节 | 16 | UINT16 | 错误码 |

**CAN 帧布局 (12 bytes):**

```
Byte 0-3:   位置 (REAL32, little-endian)
Byte 4-7:   时间戳 (UINT32, little-endian)
Byte 8-9:   扭矩 (INT16, little-endian)
Byte 10-11: 错误码 (UINT16, little-endian)
```

**TPDO1 映射配置代码** :

```cpp
// 禁用 TPDO
sdo.dl_u32(id, 0x1800, 1, 0xC0000180 | id);  // COB-ID = disabled
// 映射 4 个对象
sdo.dl_u32(id, 0x1A00, 1, 0x60640020);  // 位置 REAL32
sdo.dl_u32(id, 0x1A00, 2, 0x10130020);  // 时间戳 UINT32
sdo.dl_u32(id, 0x1A00, 3, 0x60770010);  // 扭矩 INT16
sdo.dl_u32(id, 0x1A00, 4, 0x603F0010);  // 错误码 UINT16
sdo.dl_u8 (id, 0x1A00, 0, 4);            // 映射条目数 = 4
// 参数配置
sdo.dl_u8 (id, 0x1800, 2, 255);          // 传输类型: 异步
sdo.dl_u16(id, 0x1800, 3, 5);            // 抑制时间: 5 × 0.1ms = 0.5ms
sdo.dl_u16(id, 0x1800, 5, 1);            // 事件定时器: 1ms
// 启用 TPDO
sdo.dl_u32(id, 0x1800, 1, 0x40000180 | id);
```

**接收端解析** :

MotorState 更新字段：`position_rev`, `timestamp_us`, `raw_torque_permille`, `error_code`, `multi_turns`

---

### 4.2 TPDO2 — 状态/诊断帧（8 字节）

**COB-ID:** `0x280 + Node-ID`  
**发送参数:** 传输类型 255 (异步/事件驱动), 抑制时间 19ms, 事件定时器 20ms

| 子索引 | 对象字典 | 长度 | 位宽 | 数据类型 | 描述 |
|--------|----------|------|------|----------|------|
| 1 | `0x6041:00` | 2 字节 | 16 | UINT16 | 状态字 (CiA402) |
| 2 | `0x2204:01` | 2 字节 | 16 | INT16 | 驱动器温度 (×0.1°C) |
| 3 | `0x2204:02` | 2 字节 | 16 | INT16 | 电机温度 (×0.1°C) |
| 4 | `0x6040:00` | 2 字节 | 16 | UINT16 | 控制字回显 |
| 5 | `0x603F:00` | 2 字节 | 16 | UINT16 | 错误码 |

**CAN 帧布局 (8 bytes):**

```
Byte 0-1:  状态字 (UINT16, LE)
Byte 2-3:  驱动器温度 (INT16, LE)
Byte 4-5:  电机温度 (INT16, LE)
Byte 6-7:  控制字回显 (UINT16, LE)
```

**TPDO2 映射配置代码** :

```cpp
// 禁用 TPDO
sdo.dl_u32(id, 0x1801, 1, 0xC0000280 | id);
// 映射 5 个对象
sdo.dl_u32(id, 0x1A01, 1, 0x60410010);  // 状态字 UINT16
sdo.dl_u32(id, 0x1A01, 2, 0x22040110);  // 驱动器温度 INT16 (0.1°C)
sdo.dl_u32(id, 0x1A01, 3, 0x22040210);  // 电机温度 INT16 (0.1°C)
sdo.dl_u32(id, 0x1A01, 4, 0x60400010);  // 控制字回显 UINT16
sdo.dl_u32(id, 0x1A01, 5, 0x603F0010);  // 错误码 UINT16
sdo.dl_u8 (id, 0x1A01, 0, 5);
// 参数配置
sdo.dl_u8 (id, 0x1801, 2, 255);          // 传输类型: 异步
sdo.dl_u16(id, 0x1801, 3, 190);          // 抑制时间: 190 × 0.1ms = 19ms
sdo.dl_u16(id, 0x1801, 5, 20);           // 事件定时器: 20ms
// 启用 TPDO
sdo.dl_u32(id, 0x1801, 1, 0x40000280 | id);
```

**接收端解析** :

MotorState 更新字段：`status_word`, `driver_temp_x10`, `motor_temp_x10`, `control_word`

---

## 5. RPDO 映射（主站 → 电机）

### 5.1 共享 COB-ID 策略

所有电机**共用同一个 RPDO COB-ID `0x190`**，实现一对多广播。CAN-FD 帧按各电机数据**顺序紧凑排列**，每电机根据自身模式占用不同字节数（MIT=8 字节，Velocity=6 字节）。电机根据自身在总线上的编号 `index (0..N-1)` 计算帧内偏移量，在 RPDO 映射中用 padding 对象跳过其他电机的数据。

```
CAN-FD 帧 (最大 64 字节, 顺序紧凑排列):
┌──────────────┬────────────────┬─────┐
│ Motor 0      │ Motor 1        │ ... │
│ 8 or 6 bytes │ 8 or 6 bytes   │     │
└──────────────┴────────────────┴─────┘
```

**OD entry 编码（PDO映射部分）:** `(index << 16) | (sub << 8) | bitlen`

Padding 对象：
| Padding | OD entry 值 | 说明 |
|---------|------------|------|
| PAD_U32 | `0x30000320` | 4 字节填充 (对象字典 `0x3000:03`, 32-bit) |
| PAD_U16 | `0x30000210` | 2 字节填充 (对象字典 `0x3000:02`, 16-bit) |

---

### 5.2 MIT 模式 RPDO

每电机占 8 字节固定槽位，承载打包后的 MIT 5 维目标值。

| 子索引 | 对象字典 | 长度 | 位宽 | 说明 |
|--------|----------|------|------|------|
| 1 | `0x2004:02` | 4 字节 | 32 | MIT 目标低 32 位 (torque + kd + kp 低 8 位) |
| 2 | `0x2004:03` | 4 字节 | 32 | MIT 目标高 32 位 (kp 高 4 位 + velocity + position) |

**对象字典 `0x2004` 子索引 (MIT 模式):**

| 子索引 | 名称 | 类型 | 说明 |
|--------|------|------|------|
| `0x01` | 使能 | UINT8 | MIT 控制使能标志 |
| `0x02` | 目标值低 32 位 | UINT32 | 打包后的 MIT 目标低位 |
| `0x03` | 目标值高 32 位 | UINT32 | 打包后的 MIT 目标高位 |
| `0x04` | position_min | REAL32 | 位置下限 (rev) |
| `0x05` | position_max | REAL32 | 位置上限 (rev) |
| `0x06` | velocity_min | REAL32 | 速度下限 (rev/s) |
| `0x07` | velocity_max | REAL32 | 速度上限 (rev/s) |
| `0x08` | kp_min | REAL32 | Kp 下限 (Nm/rev) |
| `0x09` | kp_max | REAL32 | Kp 上限 (Nm/rev) |
| `0x0A` | kd_min | REAL32 | Kd 下限 (Nm·s/rev) |
| `0x0B` | kd_max | REAL32 | Kd 上限 (Nm·s/rev) |
| `0x0C` | torque_min | REAL32 | 扭矩下限 (Nm) |
| `0x0D` | torque_max | REAL32 | 扭矩上限 (Nm) |
| `0x0E` | KP/KD 扭矩限幅 | UINT16 | KP/KD 输出扭矩限制 (‰) |

**默认映射范围** :

| 参数 | 最小值 | 最大值 |
|------|--------|--------|
| 位置 (rev) | -0.5 | +0.5 |
| 速度 (rev/s) | -10.0 | +10.0 |
| 扭矩 (Nm) | -10.0 | +10.0 |
| Kp (Nm/rev) | 0.0 | 100.0 |
| Kd (Nm·s/rev) | 0.0 | 20.0 |

---

### 5.3 Velocity 模式 RPDO

每电机占 **6 字节**（2 字节扭矩限幅 + 4 字节目标速度，无填充）：

| 子索引 | 对象字典 | 长度 | 位宽 | 说明 |
|--------|----------|------|------|------|
| 1 | `0x6072:00` | 2 字节 | 16 | 最大扭矩限幅 (‰ 额定扭矩) |
| 2 | `0x60FF:00` | 4 字节 | 32 | 目标速度 (REAL32, rev/s) |

**Velocity 模式 CAN 帧 slot 布局 (6 bytes):**

```
Byte 0-1:  扭矩限幅 (UINT16, LE)
Byte 2-5:  目标速度 (REAL32, LE)
```

> **设计要点:** Velocity 模式下扭矩与速度顺序映射，无需在两者之间插入填充。每电机的填充仅用于跳过帧内其他电机的数据，统一放在电机数据的首尾。

---

### 5.4 RPDO 多电机映射示例

#### 示例 A: 4 电机全部 Velocity 模式

CAN-FD 帧共 24 字节（4 电机 × 6 字节），布局如下：

```
Byte  0-1:  M1 扭矩限幅 (UINT16)
Byte  2-5:  M1 目标速度 (REAL32)
Byte  6-7:  M2 扭矩限幅 (UINT16)
Byte  8-11: M2 目标速度 (REAL32)
Byte 12-13: M3 扭矩限幅 (UINT16)
Byte 14-17: M3 目标速度 (REAL32)
Byte 18-19: M4 扭矩限幅 (UINT16)
Byte 20-23: M4 目标速度 (REAL32)
```

各电机 RPDO 映射（`0x1600`），每电机 7 个子索引：

**Motor 1 (Node-ID=1, offset=0, 本电机数据 6 字节后 18 字节填充):**

```
Sub 1: 0x6072:00  (扭矩限幅 — 本电机)
Sub 2: 0x60FF:00  (目标速度 — 本电机)
Sub 3: PAD_U32    (跳过 M2/M3/M4, 填充)
Sub 4: PAD_U32
Sub 5: PAD_U32
Sub 6: PAD_U32
Sub 7: PAD_U16
```

**Motor 2 (Node-ID=2, offset=6, 前 6 字节填充 + 本电机 6 字节 + 后 12 字节填充):**

```
Sub 1: PAD_U32    (跳过 M1, 填充)
Sub 2: PAD_U16
Sub 3: 0x6072:00  (扭矩限幅 — 本电机)
Sub 4: 0x60FF:00  (目标速度 — 本电机)
Sub 5: PAD_U32    (跳过 M3/M4, 填充)
Sub 6: PAD_U32
Sub 7: PAD_U32
```

**Motor 3 (Node-ID=3, offset=12, 前 12 字节填充 + 本电机 6 字节 + 后 6 字节填充):**

```
Sub 1: PAD_U32    (跳过 M1/M2, 填充)
Sub 2: PAD_U32
Sub 3: PAD_U32
Sub 4: 0x6072:00  (扭矩限幅 — 本电机)
Sub 5: 0x60FF:00  (目标速度 — 本电机)
Sub 6: PAD_U32    (跳过 M4, 填充)
Sub 7: PAD_U16
```

**Motor 4 (Node-ID=4, offset=18, 前 18 字节填充 + 本电机 6 字节):**

```
Sub 1: PAD_U32    (跳过 M1/M2/M3, 填充)
Sub 2: PAD_U32
Sub 3: PAD_U32
Sub 4: PAD_U32
Sub 5: PAD_U16
Sub 6: 0x6072:00  (扭矩限幅 — 本电机)
Sub 7: 0x60FF:00  (目标速度 — 本电机)
```

#### 示例 B: 混合模式 (MIT + Velocity + MIT)

假设 3 个电机：Motor 0 (MIT, 8B), Motor 1 (Velocity, 6B), Motor 2 (MIT, 8B)。帧总长 22 字节。

**Motor 0 (Node-ID=1) 的 RPDO 映射 `0x1600`:**

```
Sub 1: 0x2004:02  (MIT 低位 — 本电机)
Sub 2: 0x2004:03  (MIT 高位 — 本电机)
Sub 3: PAD_U32    (跳过 M1/M2, 填充 14B)
Sub 4: PAD_U32
Sub 5: PAD_U32
Sub 6: PAD_U16
```

**Motor 1 (Node-ID=2) 的 RPDO 映射 `0x1600`:**

```
Sub 1: PAD_U32    (跳过 M0, 填充 8B)
Sub 2: PAD_U32
Sub 3: 0x6072:00  (扭矩限幅 — 本电机)
Sub 4: 0x60FF:00  (目标速度 — 本电机)
Sub 5: PAD_U32    (跳过 M2, 填充 8B)
Sub 6: PAD_U32
```

**Motor 2 (Node-ID=3) 的 RPDO 映射 `0x1600`:**

```
Sub 1: PAD_U32    (跳过 M0/M1, 填充 14B)
Sub 2: PAD_U32
Sub 3: PAD_U32
Sub 4: PAD_U16
Sub 5: 0x2004:02  (MIT 低位 — 本电机)
Sub 6: 0x2004:03  (MIT 高位 — 本电机)
```

> **设计要点:** 各电机帧偏移量由前方电机的模式动态决定（MIT=8B, Velocity=6B）。所有电机的 RPDO 映射总长度一致（均为帧总长），由 `frame_size` 统一计算，保证 DLC 一致。

---

## 6. MIT 目标值编码

### 6.1 打包算法

MIT 模式的 5 个维度（位置、速度、扭矩、Kp、Kd）经过 clamp → 量化 → 位拼接后打包为 8 字节：

```
量化位宽分配:
  position: 16 bits
  velocity: 12 bits
  torque:   12 bits
  kp:       12 bits
  kd:       12 bits
  总计:     64 bits = 8 bytes
```

**打包代码** :

```cpp
void hexfellow_mit_target_pack(const mit_target_t* t, const mit_mapping_t* m,
                               uint8_t out[8])
{
    // 1. Clamp 到各自范围
    float position = clamp(t->position, m->position_min, m->position_max);
    float velocity = clamp(t->velocity, m->velocity_min, m->velocity_max);
    float torque   = clamp(t->torque,   m->torque_min,   m->torque_max);
    float kp       = clamp(t->kp,       m->kp_min,       m->kp_max);
    float kd       = clamp(t->kd,       m->kd_min,       m->kd_max);

    // 2. 量化
    uint32_t pos_u  = float_to_uint(position, m->position_min, m->position_max, 16);
    uint32_t vel_u  = float_to_uint(velocity, m->velocity_min, m->velocity_max, 12);
    uint32_t torq_u = float_to_uint(torque,   m->torque_min,   m->torque_max,   12);
    uint32_t kp_u   = float_to_uint(kp,       m->kp_min,       m->kp_max,       12);
    uint32_t kd_u   = float_to_uint(kd,       m->kd_min,       m->kd_max,       12);

    // 3. 位拼接
    uint32_t lower_u32 = torq_u | (kd_u << 12) | ((kp_u & 0xFFu) << 24);
    uint32_t upper_u32 = (kp_u >> 8) | (vel_u << 4) | (pos_u << 16);

    // 4. 小端存储
    store_u32_le(out,     lower_u32);
    store_u32_le(out + 4, upper_u32);
}
```

### 6.2 位布局

**低 32 位 (bytes 0-3):**
```
Bit 0-11:   torque  (12 bits)
Bit 12-23:  kd      (12 bits)
Bit 24-31:  kp[0:7] (低 8 bits)
```

**高 32 位 (bytes 4-7):**
```
Bit 0-3:    kp[8:11] (高 4 bits)
Bit 4-15:   velocity (12 bits)
Bit 16-31:  position (16 bits)
```

### 6.3 量化公式

```
uint_value = (float_value - min) × (2^bits - 1) / (max - min)
```

利用 `float_to_uint`  实现。

---

## 7. 初始化流程

每个电机的初始化序列 (`initOneMotor`) 按以下步骤执行：

```
1.  NMT → Pre-Operational           (允许 SDO 配置)
2.  读取 0x1018:01 — 验证工厂 UID   (0x4859444C)
3.  读取 0x1018:03 — 验证固件版本   (v8以上)
4.  读取 0x1018:04 — 记录序列号
5.  读取 0x6076:00 — 读取峰值扭矩   (REAL32, Nm)
6.  写入 0x2040:00 — 失能时短接制动
7.  写入 0x6072:00 — 最大扭矩限幅
8.  写入 0x1016:01 — 禁用消费者心跳监控
9.  写入 0x6040:00 — CiA402 状态机复位 (0 → 0x80)
10. 写入 0x6060:00 — 设置工作模式 (MIT=5, Velocity=3)
11. [if(MIT 模式)] 配置 0x2004 子索引 (范围 + KP/KD 限幅)
12. 配置 TPDO1 / TPDO2 映射
13. 配置 RPDO1 映射 (共享 COB-ID 0x190 + 动态偏移填充)
14. CiA402 状态机: Shutdown(6) → SwitchOn(7) → Enable(0xF)
15. NMT → Operational                (PDO 开始传输)
16. 写入 0x1016:01 — 启用消费者心跳监控 (超时 250ms)
```

所有电机初始化完成后，`HexfellowMotorTask::main()` 通过二进制信号量 (`init_semaphore_`) 通知 `app_main`：
- `app_main` 在调用 `hexmotor_task.start()` 后立即 `waitForInit(1500)` 阻塞等待
- 若 1500ms 内未收到信号量（初始化失败或超时），`app_main` 输出错误并退出主循环
- 初始化成功则进入事件驱动主循环

---

## 8. 运行时控制环路

### 8.1 控制任务主循环

`HexfellowMotorTask::main()` 以 1ms 周期运行，启动时先执行 CANopen 初始化并通知 `app_main`：

```
启动:
  ├── 重绑定 CAN RX 信号到本任务句柄 (SDO 操作需要)
  ├── controller_.init(sdo, driver)  — CANopen SDO 初始化所有电机
  ├── xSemaphoreGive(init_semaphore_) — 通知 app_main 初始化完成
  └── 进入 1ms 定时循环

1ms 定时循环:
┌──────────────────────────────────────────┐
│           1ms 定时循环                    │
│                                          │
│  ┌──────────────────────────────────┐    │
│  │  每 50ms: 发送主站心跳 (0x705)   │    │
│  └──────────────────────────────────┘    │
│  ┌──────────────────────────────────┐    │
│  │  构建并发送 RPDO 帧 (0x190)      │    │
│  │  - MIT 电机: pack 5 维目标值     │    │
│  │  - Vel 电机: 扭矩限幅 + 目标速度  │    │
│  └──────────────────────────────────┘    │
│  ┌──────────────────────────────────┐    │
│  │  处理接收到的 TPDO 帧            │    │
│  │  - TPDO1: 位置/时间戳/扭矩/错误  │    │
│  │  - TPDO2: 状态/温度/控制字       │    │
│  └──────────────────────────────────┘    │
└──────────────────────────────────────────┘

app_main 主循环 (事件驱动):
┌──────────────────────────────────────────┐
│  ulTaskNotifyTake(500ms)                 │
│    ├── 有 spd 命令 → 更新电机 0 速度     │
│    └── 超时 → 继续循环                   │
│  hexmotor_task.setMitTarget(0, target)   │
│  hexmotor_task.snapshot(0, state)        │
│  日志输出电机状态                         │
└──────────────────────────────────────────┘
```

### 8.2 线程安全

**电机控制路径（`std::mutex` 保护）：**
- `setMitTarget()` / `setVelocityTarget()` — app_main 写入目标值
- `buildRpdoFrame()` — 控制线程读取目标值构建 RPDO
- `handleRxFrame()` — 控制线程更新 MotorState
- `snapshot()` — app_main 读取 MotorState

---

## 9. MotorState 完整字段

```cpp
struct MotorState {
    float    position_rev;        // 0x6064 — 实际位置 (rev)
    int32_t  multi_turns;         // 本地累积多圈计数
    uint32_t timestamp_us;        // 0x1013 — 高速时间戳 (μs)
    int16_t  raw_torque_permille; // 0x6077 — 实际扭矩 (‰)
    uint16_t error_code;          // 0x603F — 错误码

    uint16_t status_word;         // 0x6041 — CiA402 状态字
    int16_t  driver_temp_x10;     // 0x2204:01 — 驱动器温度 (×0.1°C)
    int16_t  motor_temp_x10;      // 0x2204:02 — 电机温度 (×0.1°C)
    uint16_t control_word;        // 0x6040 — 控制字回显

    uint64_t last_tpdo1_ms;       // 最后一次 TPDO1 接收时间
    uint64_t last_tpdo2_ms;       // 最后一次 TPDO2 接收时间
};
```

---

## 10. 类继承关系

```
BasicObject
    └── ICanDriver (接口)
            └── Esp32CanFdDriver    — CAN-FD 硬件驱动

AppTask
    └── HexfellowMotorTask          — FreeRTOS 控制任务
            ├── co_master_sdo       — CANopen SDO 主机端
            └── HexfellowMotorController — 核心控制器
```

---

## 11. PDO 映射汇总表

| PDO | COB-ID | 长度 | 周期/触发 | 内容 |
|-----|--------|------|-----------|------|
| TPDO1 | `0x180 + NID` | 12 B | ~1ms 事件 | 位置, 时间戳, 扭矩, 错误码 |
| TPDO2 | `0x280 + NID` | 8 B | ~20ms 事件 | 状态字, 温度, 控制字回显, 错误码 |
| RPDO1 | `0x190` | Σ(电机数据) | 1ms 同步 | MIT: 5 维打包 8B / Vel: 扭矩 2B + 速度 4B, 顺序紧凑排列 |

---