# Object dictionary

下表是当前 `StandardProfile` 的稳定公共对象。PDO communication/mapping 对象按 4 通道展开；默认 mapping count 为 0，通信复位会恢复表中的默认值。

| Index | Sub | Type | Access | PDO | Meaning / default |
|---:|---:|---|---|---|---|
| `1000` | 0 | UNSIGNED32 | ro | no | Device type, 0 |
| `1001` | 0 | UNSIGNED8 | ro | yes | Error register, 0 |
| `1005` | 0 | UNSIGNED32 | ro | no | SYNC COB-ID, `0x80` |
| `1006` | 0 | UNSIGNED32 | rw | no | Communication cycle period, 0 |
| `1008` | 0 | VISIBLE_STRING | ro | no | Device name |
| `1009` | 0 | VISIBLE_STRING | ro | no | Hardware version |
| `100A` | 0 | VISIBLE_STRING | ro | no | Software version |
| `1010` | 0 | UNSIGNED8 | ro | no | Store parameter entry count, 1 |
| `1010` | 1 | UNSIGNED32 | rw | no | Read capability `1`; write `0x65766173` (`save`) to commit |
| `1013` | 0 | UNSIGNED32 | ro | yes | High-resolution timestamp, low 32 bits in µs |
| `1016` | 0 | UNSIGNED8 | ro | no | Consumer heartbeat entry count, 4 |
| `1016` | 1–4 | UNSIGNED32 | rw | no | Consumer heartbeat parameters; supervision pending |
| `1017` | 0 | UNSIGNED16 | rw | no | Producer heartbeat time, default 100 ms |
| `1018` | 0 | UNSIGNED8 | ro | no | Identity entry count, 4 |
| `1018` | 1–4 | UNSIGNED32 | ro | no | Vendor/product/revision/serial from Kconfig |
| `1200` | 0 | UNSIGNED8 | ro | no | SDO server parameter count, 2 |
| `1200` | 1 | UNSIGNED32 | ro | no | Client-to-server COB-ID, `0x600 + Node-ID` |
| `1200` | 2 | UNSIGNED32 | ro | no | Server-to-client COB-ID, `0x580 + Node-ID` |
| `1400`–`1403` | 0–2 | record | mixed | no | RPDO communication parameters |
| `1600`–`1603` | 0–16 | record | rw | no | RPDO mapping, max 16 entries / 64 bytes |
| `1800`–`1803` | 0–5 | record | mixed | no | TPDO communication parameters |
| `1A00`–`1A03` | 0–16 | record | rw | no | TPDO mapping, max 16 entries / 64 bytes |
| `2000` | 0 | UNSIGNED8 | ro | no | Manufacturer record entry count, 4 |
| `2000` | 1 | UNSIGNED32 | ro | yes | Uptime in ms |
| `2000` | 2 | UNSIGNED32 | ro | yes | Successfully applied RPDO count |
| `2000` | 3 | UNSIGNED32 | ro | yes | Successfully queued TPDO count |
| `2000` | 4 | UNSIGNED32 | rw | yes | Example application value |
| `2001` | 0 | UNSIGNED8 | ro | no | Node configuration entry count, 1 |
| `2001` | 1 | UNSIGNED8 | rw | no | Node-ID for next restart, valid range `1..127` |

PDO mapping word follows CiA 301 layout: `(index << 16) | (subindex << 8) | bit_length`。例如 `0x20000420` 表示把 `0x2000:04` 的 32 bits 放入 PDO。

合法的动态映射顺序是：先把 mapping subindex 0 写为 0，再写 subindex 1..N，最后把 subindex 0 写为 N。如果需要修改一个仍启用的 COB-ID，必须先通过 bit 31 disable，再改 ID，然后重新 enable。

Node-ID 保存采用两阶段语义：先写 `0x2001:01` 只改变 RAM 中的待保存值，随后写 `0x1010:01 = 0x65766173` 才提交 NVS。保存响应发出后当前通信仍使用旧 Node-ID；断电重上电或 CPU 复位后，Boot-up、Heartbeat、SDO 与默认 PDO COB-ID 才使用新值。
