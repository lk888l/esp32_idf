# USB / Wi-Fi / BLE 网关

StickS3 通过现有 USB 串口、HTTP 或加密 BLE 控制通道，操作一个用户选定的外部
BLE 外设：发现主服务、特征和描述符，读取属性，确认式写入，订阅通知或指示。
三条控制通道共用 API v1、有界队列和同一个 NimBLE host。

这是 BLE GATT 网关，电脑使用串口或配套客户端，不会枚举出系统原生蓝牙适配器。
不提供经典蓝牙 SPP、音频、通用文件传输或厂商业务协议的自动解析。

## USB 与 SWD

USB 复用 ESP32-S3 自带 Serial/JTAG 的 CDC-ACM 串口。Linux 通常为 `/dev/ttyACM*`，
macOS 为 `/dev/cu*`，Windows 为 `COM*`，见
[ESP-IDF 5.5 USB Serial/JTAG](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/usb-serial-jtag-console.html)。
预期使用系统自带 CDC 驱动，Windows 自动绑定机制见
[Microsoft Usbser.sys](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)。
本轮未进行 Windows/macOS 免驱实机验收，不保证旧驱动绑定或 WSL 转接配置即插即用。

进入 **USB DAP** 时 TinyUSB 占用内部 USB PHY，本地串口暂停；退出后恢复。
无线网关使用现有无线资源。关闭 BLE 会断开控制连接与外连设备；关闭 Wi-Fi 会中断
HTTP 控制连接。网关不占用 SWD 引脚，不修改 USB eFuse。

## 开始使用

在 BLE 页面扫描并连接外设，或通过串口操作：

```text
ble scan
ble results
ble results 0
ble connect AA:BB:CC:DD:EE:FF public --temporary
ble status
gatt status
help gatt
```

扫描完成后选择可连接设备，地址和类型取自扫描结果；等待 `connected=1`。
`gatt status` 返回当前 `generation`。下面的 `1` 仅为示例，每次重新连接后都须查询。
重启会清除会话、结果和通知。

```text
gatt services 1
gatt result 42
gatt result 42 1
gatt characteristics 1 1 20
gatt descriptors 1 7 10
gatt read 1 7
gatt write 1 7 0100ff
gatt subscribe 1 10 1
gatt events 1 0
gatt subscribe 1 10 0
```

每次操作返回独立 `ticket`，示例 `42` 须替换为实际值。轮询到 `state=complete/failed`。
发现结果用 `index` 逐行读取，长数据用 `offset` 分页。句柄必须来自实际发现结果：
读写使用 `value_handle`，订阅使用 UUID `0x2902` 的 **CCCD 句柄**，
不能假定它等于 `value_handle+1`。描述符范围从特征值句柄开始，到下一特征声明的
前一句柄结束；最后一个特征使用服务的 `end`。串口 CLI 数字为十进制。

## 电脑客户端

`tools/ble_gateway.py` 保持一次调用中的控制连接，自动轮询结果并拼接长数据。
USB 需要 `pyserial`，BLE 需要 `bleak`，HTTP 只使用标准库。传输参数互斥，
默认 HTTP 地址为 `http://192.168.4.1`。Python 客户端也接受 `0x` 十六进制句柄。

```bash
python -m pip install pyserial
python tools/ble_gateway.py --port /dev/ttyACM0 status
python tools/ble_gateway.py --port /dev/ttyACM0 services
python tools/ble_gateway.py --port /dev/ttyACM0 characteristics 1 20
python tools/ble_gateway.py --port /dev/ttyACM0 descriptors 7 10
python tools/ble_gateway.py --port /dev/ttyACM0 read 7
python tools/ble_gateway.py --port /dev/ttyACM0 write 7 0100ff
python tools/ble_gateway.py --port /dev/ttyACM0 subscribe 10 notify
python tools/ble_gateway.py --port /dev/ttyACM0 watch --seconds 60
python tools/ble_gateway.py --port /dev/ttyACM0 subscribe 10 off

# 无线访问需预先设置 M5_API_TOKEN，取自本地 USB 启动日志。
python tools/ble_gateway.py --url http://192.168.4.1 services
python tools/ble_gateway.py --ble <StickS3控制端地址> read 7
```

也提供 `scan`、`results --index N`、`connect ADDRESS --address-type N`、`disconnect`。
这些连接管理操作的回执表示进入队列，用 `status`/`results` 确认实际状态。
客户端连接默认临时，添加 `--remember` 才请求成功后记住。

每次 GATT 操作前客户端查询连接代次，随后固定使用它。跨多次命令进行业务事务时，
使用全局 `--generation N` 防止操作另一条连接。不要同时用串口监视器占用同一端口。

`watch` 每行输出一条通知 JSON，包含 `sequence`、`lost`、`indication` 和十六进制数据，
不混入固件日志。`--after N` 可从同一连接代次的游标继续。客户端退出不会自动取消
远端订阅；使用 `subscribe ... off` 或断开外设结束订阅。

## API v1

封装为 `{"v":1,"id":N,"op":"ble.gatt.…",...}`。USB 每行一个请求，响应为
`RS(0x1e)+JSON+LF`；HTTP 使用 `POST /api/v1/command`；BLE 使用现有 NUS RX/TX。
无线网关的**所有查询和修改**都要求 `token`，包括属性值和通知。USB 本地来源由
传输层确定，不能通过 JSON 字段伪造。未知字段和非法数值在排队前拒绝。

| 操作后缀 | 字段 | 行为 |
| --- | --- | --- |
| `status` | 无 | 连接代次、MTU、当前票据、事件计数、容量 |
| `services` | `generation` | 发现所有主服务，保留前 16 条 |
| `characteristics` | `generation, handle, end` | 在闭区间发现特征 |
| `descriptors` | `generation, handle, end` | 发现特征值句柄之后的描述符 |
| `read` | `generation, handle` | ATT Read/Read Blob，最多 512 字节 |
| `write` | `generation, handle, data` | 十六进制数据，Write Request，等待远端确认 |
| `subscribe` | `generation, handle, mode` | 写显式 CCCD；0 取消，1 通知，2 指示 |
| `mtu` | `generation` | 外连 MTU 交换；结果为两字节小端 MTU |
| `pair` | `generation` | 外连加密/配对，沿用 Just Works 和 bond 存储 |
| `result` | `ticket, index?, offset?` | 非消耗式结果分页，可选字段默认 0 |
| `events` | `generation, after?` | 游标之后最早仍保留的事件 |
| `events` | `generation, sequence, offset?` | 固定事件编号读取后续页 |

特征行返回 `uuid, handle, value_handle, properties`；服务返回 `handle, end`。
调用者确认远端属性权限及应用协议。`pair` 不提供键盘 passkey、数字比较或 OOB 输入。

`accepted` 表示服务队列收到请求，`command.result` 的 `applied` 表示 BLE 队列接收。
最终结果以 `ble.gatt.result` 为准：`queued → running → complete/failed`。
执行前再次核对代次，排队后切换设备也不能误写。正 `error_code` 为原始 NimBLE/ATT
状态，负数为网关错误，附 `error_name`。进入 BLE 队列前的失败使用 `error_domain=esp`。

通知查询不消费其他客户端数据。`lost` 表示游标到返回事件之间已覆盖的条数。
64 字节之后的分页必须固定 `sequence`，避免混入新通知；已覆盖返回 `event_expired`。
重新连接清空通知并递增代次，旧代次请求返回 `stale_generation`。

## 资源和失败边界

- 一个外连设备、一个上行 BLE 控制客户端，默认共两条 BLE 链接。
- 一次执行一个网关操作，服务队列和 BLE host 队列各 4 槽。
- 保留最近 4 次成功进入网关的操作结果，每次发现保留 16 行，同时返回 `total/truncated`。
  特征和描述符可缩小句柄范围重新发现；主服务超出 16 项时当前接口只保留前 16 项。
- 读取上限 512 字节，每页 64 字节；写入上限 `min(48, 外连MTU-3)`，默认 MTU 23
  最多写 20 字节。`mtu` 可协商更大值。不自动拆写，不实现 Long Write 或 Write Without Response。
- 缓存 8 条通知，每条最多 512 字节，覆盖和超限有显式标记；不承诺持续高速通知无损。
- 执行超时 15 秒，由 host 每秒巡检；超时关闭准入并请求断开，清退未结束的 ATT 事务。
  远端已写入而确认丢失仍可能超时；客户端不会自动重发，业务层应先查询实际状态。
- 状态机、协议和驱动分层；固定容量结果、通知和短锁；GATT 回调携带不可变 ticket，
  防止 HCI 句柄复用后旧回调污染新事务。远端值不写入常规固件日志或 NVS。
- 继承现有安全边界：HTTP 无 TLS，适用于受保护热点或可信局域网；
  BLE Just Works 不提供主动中间人认证。

上行 BLE 请求仍须整体适配 `MTU-3` 和 256 字节 JSON 上限，长响应可 Read Blob。
上行和外连 MTU 相互独立。NUS ready 通知只提示控制响应就绪；外设数据通过
`ble.gatt.events` 提取。

## 构建与验证

```bash
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp-idf/export.sh >/dev/null && bash tools/verify_ble_gateway.sh'
```

脚本只构建和测试，不烧录，不连接附近设备。本轮 ESP-IDF 5.5.4 正式固件构建及
15 项主机 CTest 通过；新增 C/C++ 状态机、实际 GATT 适配层、服务协议测试启用
ASan/UBSan。覆盖旧代次、HCI 句柄复用、MTU 边界、CCCD、通知覆盖、鉴权、
BLE 编译关闭、串口分帧、请求关联、分页及客户端不重发写入。

本轮未烧录，新网关尚未实机验收。待在用户可控的测试外设上检查：

1. USB 发现服务/特征/描述符，短值与 512 字节长读取。
2. MTU 23/协商 MTU 下的确认写入、订阅/取消、通知/指示。
3. Wi-Fi 与加密 BLE 控制路径，上行和外连同时工作。
4. 排队写入时切换外设、外设断电、超时、通知覆盖及代次隔离。
5. BLE 关闭/恢复、USB DAP 进入/退出、菜单响应、堆与 host 栈余量。
6. Linux、Windows、macOS CDC 枚举、MTU 和配对互操作。
