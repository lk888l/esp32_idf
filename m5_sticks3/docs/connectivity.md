# Wi-Fi / BLE 快速使用

工程仍沿用原有 AppModule / AppManager / AppTask。新增的 connectivity 组件负责通信协议，
app_modules 中的 ConnectivityModule 只接入生命周期，UI 只读取状态快照。

1. 在串口启动日志中找到 `local setup` 两行，取得设备独立的热点密码和 API 令牌。
2. 连接 `M5StickS3-xxxxxx` 热点，默认地址为 `192.168.4.1`。当前这台设备名称为
   `M5StickS3-D536F4`；SYSTEM 页面也会显示 IP 与 BLE 状态。
3. 在工程目录运行 `python tools/connectivity_client.py status`；`ping`、
   `echo hello`、`telemetry` 可直接验证通道。
4. 将串口中的令牌设为环境变量 `M5_API_TOKEN`，运行
   `python tools/connectivity_client.py wifi-set "你的2.4GHz Wi-Fi名称"`，
   按提示输入密码。配置会持久化，备用热点保持可用。`wifi-clear` 仅清除 STA 配置。
5. Windows 蓝牙客户端安装 `bleak` 后可运行
   `python tools/connectivity_client.py --ble 14:C1:9F:D5:36:F6 status`。
   它会建立加密连接并保存配对关系；需要手机自行开发客户端时，按下文 UUID 和协议实现。

配网命令的 `accepted` 表示已入队；查看 `status` 中的 `last_id/last_error`
确认执行结果，再查看 `wifi/ip` 确认是否真正连网。当前框架提供查询、回显、遥测和配网，
后续内网控制命令应继续通过同一个有界队列分发，避免直接从网络回调操作 UI 或硬件。

详细的架构、接口边界和构建说明如下。

## 本次验收记录（2026-09-05）

下表区分已经通过的检查与仍待完成的实机验证。配网命令写入成功不代表 STA
已连接路由器；只有连接状态与 DHCP 地址同时确认，才能判定 STA 连网成功。

| 验收项 | 结果与范围 |
| --- | --- |
| 正式固件 | 使用项目 Docker 中的 ESP-IDF **5.5.4** 完成正式构建和实机烧录。 |
| AP 与 DHCP | Windows 成功连接设备 AP，并取得 DHCP 地址 **192.168.4.2**；设备 AP 地址为 **192.168.4.1**。 |
| HTTP API | **31 项实机检查通过**，覆盖查询、回显、遥测、认证、配网命令队列与完成状态、STA 配置 NVS 写入，以及 JSON 和请求长度边界；最终正式固件再次通过 **27 项只读 HTTP 检查**。 |
| 主机测试 | 刷新 `build/host-tests-codex` 的 CMake 后，CTest **2/2 通过**；通信策略测试另经 **ASan / UBSan** 检查通过。 |
| 可选传输构建 | 分别关闭 Wi-Fi、BLE 的配置均构建通过。 |
| 原有功能回归 | 三款小游戏及 BMI270 **100 Hz** 采样的硬件冒烟测试通过。 |
| BLE 最终固件验证 | 正式固件经 USB 复位启动后保留已有 bond；ping、322 字节 status 长读取、128 字节 echo 与 ready 通知通过。复测的两个连续连接会话均通过，MTU 为 **259**，链路状态为 **encrypted=1 / bonded=1**。此前一次 Windows Bleak 服务发现出现 descriptor 异步结果 `AssertionError`，重试后恢复。 |
| 交付客户端 | 最终交付的 Python CLI 并行执行 HTTP 与 BLE `status` 均通过。 |
| STA 成功连接与恢复 | 使用 Windows 临时热点（请求 2.4 GHz）进行了两次测试，均返回 **201 / NO_AP_FOUND**；尚未验证成功取得 STA DHCP 地址或断网后的恢复。该结果不足以判断问题位于哪一端，需要使用实际的 2.4 GHz 路由器继续复测。 |

# Connectivity framework

## Architecture

The application retains its existing composition root and fixed-capacity
`AppManager`. The new `ConnectivityModule` delegates lifecycle operations to
`connectivity::Service`; it is registered after the board/motion/wave modules
and before the UI. The manager still initializes in order and stops in reverse.

`Service` is a facade over two transport adapters (`WifiTransport` and
`BleTransport`). Both call the same versioned request handler. Transport
callbacks can read immutable snapshots or enqueue commands into a statically
allocated four-entry queue. Only the application task applies and persists
station configuration. NimBLE manages its own peer-bond NVS storage from the
Bluetooth stack context. One command is processed per application tick. The
UI reads a short critical-section-protected POD snapshot, following `MotionState`.

Connection retry policy and credential validation are pure C++ and covered by
host tests. There are no direct LVGL, GPIO, waveform or IMU mutations from
network callbacks. `telemetry` reads the existing motion snapshot; entering or
leaving the existing motion/game pages still owns sensor activation.

## First boot and configuration

With no saved station credentials, Wi-Fi starts a protected access point:

- SSID and BLE name: `M5StickS3-<last 3 MAC bytes>`
- Access point address: `192.168.4.1`
- Password: independently generated 16 hexadecimal characters
- API token: independently generated 32 hexadecimal characters
- Both secrets persist in the `stick_net` NVS namespace and appear only on the
  physical USB console as `local setup` lines. They are never returned by the API.

Read the USB console, connect a computer/phone to that AP, then configure the
station. The AP remains available while the station connects or retries.
Wi-Fi is 2.4 GHz; the station accepts an open network (explicit empty password),
an 8–63 byte passphrase or a 64-character hexadecimal PSK. SSIDs are 1–32 bytes.
A failed station connection uses 1, 2, 4, 8, 16, 32, then 60-second retries.
Acquiring an address resets the retry delay.

`wifi.clear` clears only the saved station credentials. It preserves the AP
password, API token and other components' NVS data. Corrupt/incompatible NVS
is reported as an initialization error rather than automatically erased.
Never erase all flash just to change station credentials.

The SYSTEM page shows the station IP when connected, otherwise the AP IP,
and BLE readiness/connection status. Heap, PSRAM and uptime remain visible.

## API v1

One request is one UTF-8 JSON object, at most **256 bytes**. Every request has
`v: 1`, an integer `id` from 0 through 2147483647, and `op`.
IDs correlate replies; they are not an exactly-once delivery mechanism.
Replies include `v`, `id` and `ok`. Failed requests include `error`.
Duplicate keys, trailing JSON, embedded NULs and excessive nesting are rejected.

| Operation | Additional fields | Behavior |
| --- | --- | --- |
| `status` | none | Wi-Fi/AP/BLE status, heap, uptime, command progress |
| `ping` | none | `reply: "pong"` |
| `echo` | `data` (up to 128 UTF-8 bytes) | Return application data |
| `telemetry` | none | Latest motion validity, count and Euler angles |
| `wifi.configure` | `token`, `ssid`, `password` | Persist and apply station settings asynchronously |
| `wifi.clear` | `token` | Clear saved station settings asynchronously |

For mutations, `{"result":"accepted"}` means queued, not connected or durable.
Poll `status`: `completed` is a monotonically increasing completion counter;
`last_id` and `last_error` describe the most recently processed command.
`last_error: 0` means settings were persisted/applied to the driver; check
`wifi: "connected"` and `ip` for actual DHCP success. Multiple writers must
coordinate because only the latest completion is retained. Queue saturation
returns `busy`; callers should retry with bounded backoff.

Stored changes are committed before reconnecting. If the driver fails after a
commit, the saved settings remain and will be used on restart. Repeating a
configuration reapplies it while avoiding an unnecessary identical NVS write.

### HTTP transport

- `GET /api/v1/status`
- `POST /api/v1/command`, `Content-Type: application/json`
- Available on AP and, once connected, the station address.
- No CORS grant. Error responses and socket/request limits are bounded.
- Logical protocol errors use an `ok:false` JSON response; transport errors
  additionally use the appropriate HTTP status code.

Example:

```json
{"v":1,"id":42,"op":"wifi.configure","token":"<USB-console-token>","ssid":"Home","password":"<WiFi-password>"}
```

### BLE transport

ESP32-S3 supports BLE; this framework uses IDF's NimBLE peripheral stack and
the Nordic UART Service UUID convention:

| Attribute | UUID | Properties |
| --- | --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | Primary |
| RX | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | Encrypted Write Request |
| TX | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Encrypted Read / Notify |

Each RX write contains one complete JSON request. Use write-with-response;
the request must fit both 256 bytes and `negotiated MTU - 3`. The preferred
MTU is 259. Fragmented or prepared writes are not an application framing
protocol: negotiate sufficient MTU or shorten the request.

After RX succeeds, read TX (Read Blob / long read as necessary). TX holds the
latest complete response, up to 512 bytes. Subscribing to TX gives a compact
`{"ready":N}` notification, where N is the response length; it does not contain
the full response. If a notification is lost, reading TX still retrieves it.
Finish reading before sending another request. Disconnect clears the response
and restarts advertising. Only one BLE peer is supported.

The link uses Secure Connections with Just Works encryption and persists peer
bonds in NVS for encrypted reconnects. Legacy pairing and debug keys are disabled.
Only the current peer can replace its stale bond; a full bond store returns an
error rather than silently removing another peer. The API token still authorizes writes. This protects against passive
radio capture, but Just Works does not authenticate the peer against active
man-in-the-middle attacks. HTTP has no TLS. This initial framework is intended
for the password-protected AP and trusted local networks; an internet-facing
deployment needs authenticated secure provisioning, TLS and an explicit access
policy. NVS is not encrypted by this development configuration.

### Python client and regression tests

The HTTP client uses only Python's standard library. BLE additionally requires
`bleak` on the computer running the client. Bluetooth remains attached to the
Windows host; only the ESP32 USB programming interface is mapped into WSL.

```bash
python tools/connectivity_client.py --help
python tools/connectivity_client.py --url http://192.168.4.1 status
python tools/connectivity_client.py --url http://192.168.4.1 ping
python tools/connectivity_client.py --url http://192.168.4.1 echo hello
python tests/connectivity_api_test.py --url http://192.168.4.1
```

Use `M5_API_TOKEN` for the API token. The client accepts Wi-Fi passwords through
a prompt or `M5_WIFI_PASSWORD`, keeping secrets out of source control and the
command line. See `--help` for BLE and provisioning operations. The API test is
read-only by default; its explicit `--exercise-wifi` mode temporarily replaces
station configuration and finally clears it, so use that mode only on a
development device with no station settings to preserve.

The repeatable BLE hardware regression uses the same optional Bleak dependency.
Turn on host Bluetooth first; the test does not change host radio settings or
erase bonds. Run it again after a device reset to check saved-bond restoration:

```bash
python tests/connectivity_ble_test.py --ble 14:C1:9F:D5:36:F6
```

Its default two sessions verify pairing/encryption, full responses exceeding
the negotiated MTU, a 128-byte echo and each corresponding ready notification.
Use `--name` or `--ble` to select a different device.

## Build and hardware access

Keep the repository's ESP-IDF **5.5.4** container rather than upgrading the SDK
during a connectivity feature change. `sdkconfig.defaults` enables NimBLE,
one BLE connection, Wi-Fi/BLE coexistence and an 8192-byte NimBLE host stack.
NimBLE and suitable Wi-Fi/LwIP allocations use PSRAM, and 64 KiB of internal
RAM is reserved for internal/DMA allocations. BSP linker rules move LVGL's
existing fixed 96 KiB TLSF arena into PSRAM without changing its allocator or
the 2×60-line DMA display buffers. Keep the SPI interrupt at `intr_flags=0`:
its completion callback touches LVGL objects and must be masked during flash
cache suspension for NVS writes.
The custom partition table keeps NVS and the factory offset unchanged and
provides a 3 MiB factory app slot. Remaining flash is reserved; OTA is not
implemented by this change.

```bash
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp/idf/export.sh && idf.py -B build/codex-idf build'
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp/idf/export.sh && idf.py -B build/codex-idf -p /dev/ttyACM0 flash'
```

Use menuconfig's **M5-StickS3 connectivity** to disable either transport.
Turning both off leaves the pre-existing application usable.

On Windows, inspect `usbipd list` and attach the actual ESP32 BUSID:

```powershell
usbipd attach --wsl --busid <actual-BUSID>
```

If the already-running privileged container lacks the device, inspect
`ls -l /dev/ttyACM0` in WSL and create the matching character device inside the
container using its actual major/minor numbers. Do not map unrelated USB devices.

## Extension points

Add future commands to the common request handler, validate into a fixed-size
command payload, and dispatch the domain action from the application task.
If a command needs another thread, use an explicit request interface and a
state snapshot, as the existing motion module does. Do not call
`wave::Generator` or LVGL directly from an HTTP/NimBLE callback.

Large file transfers need their own chunk framing, sequence numbers, integrity
checks, cancellation and flow control; the current request/response channel
is deliberately bounded. Bulk transfer, OTA, cloud access and remote actuator
commands are not yet implemented.
