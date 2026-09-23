# 本地无线命令与 USB 协议

正式固件现在支持通过 StickS3 自带的 **USB Serial/JTAG 串口**，在运行中扫描、
连接、切换、断开 Wi-Fi/BLE，以及保存和忘记连接。图形菜单、串口、HTTP、BLE
共用 `connectivity::Service` 的有界命令队列；扫描和连接在后台执行。
BLE 数据操作和配套电脑客户端见 [BLE 网关](ble-gateway.md)，串口输入 `help gatt` 获取帮助。

## 开始使用

使用 `idf.py monitor` 或串口终端连接设备。常规串口设置可选 115200/8N1，
USB Serial/JTAG 本身不依赖这个波特率。发送一行命令，并以 LF、CR 或 CRLF 结束。
启动日志中出现 `radio_console: ready` 后可以使用；应用尚未完全初始化时返回
`unavailable`。输入 `help`、`help wifi`、`help ble`、`help protocol` 获取帮助。
某些终端在打开端口时切换 DTR/RTS，会触发复位。自动测试先等待启动，并以只读
`ping` 确认就绪；不要因启动阶段丢失响应而自动重发连接、忘记等修改命令。

```text
help
capabilities
status
wifi status
wifi scan
wifi results
wifi results 1
wifi connect "家里的 Wi-Fi" "your-password"
wifi status
wifi saved
wifi saved 1
wifi disconnect
wifi use 0
wifi forget 0
```

`wifi scan` 返回的 `accepted` 仅表示命令已进入队列。用响应中的 `ticket`
执行 `command <ticket>` 查询驱动是否接受扫描，然后重复 `wifi results`，直到
`scanning=0`、`generation` 更新，再按 `index=0..count-1` 读取结果。
BLE 扫描同理。无加密 Wi-Fi 使用显式空密码：`wifi connect "Open AP" ""`。

命令名区分大小写；名称、密码支持 UTF-8、单引号或双引号。反斜杠可转义
反斜杠、引号或空格。密码中的 `$`、反引号、分号均是普通字符，不会执行 shell。
支持退格，不提供设备端输入回显或历史记录，避免把密码再次输出或保存在历史中。
如果使用终端的本地回显/日志记录功能，其内容由终端保存。

## 命令表

| 命令 | 含义 |
| --- | --- |
| `help [wifi\|ble\|gatt\|protocol]` | 分主题帮助 |
| `capabilities` | API 版本、编译启用的无线、请求/响应上限、队列/记忆容量 |
| `status`、`ping`、`traffic` | 总状态、通道验证、HTTP/BLE/串口应用流量 |
| `command TICKET` | 查询最近 16 个已接收命令中的一个 |
| `wifi status` | STA 状态、SSID、IP、RSSI、断开原因、待保存状态和存储错误 |
| `wifi scan`、`wifi results [INDEX]` | 发起扫描，读取元数据和一条结果 |
| `wifi connect SSID PASSWORD [--remember\|--temporary]` | 切换当前连接；默认连接成功后记住 |
| `wifi saved [SLOT]` | 列出已使用槽位及指定槽内容，永不返回密码 |
| `wifi use SLOT` | 用保存的凭据连接；成功后将它设为启动首选 |
| `wifi reconnect` | 重新连接启动首选网络；没有首选时返回未找到 |
| `wifi remember` | 将当前已经取得 IP 的网络保存为首选 |
| `wifi forget SLOT\|all` | 删除指定/全部记忆，并取消相关待保存请求 |
| `wifi disconnect` | 停止当前 STA 的连接/重试；保留记忆和备用热点 |
| `wifi on`、`wifi off` | 运行时启用/关闭整个 Wi-Fi，包括备用热点 |
| `ble status`、`ble peer [INDEX]` | 主动连接状态/身份/保存结果，以及主服务 UUID |
| `ble scan`、`ble results [INDEX]` | 扫描 BLE 广播设备及可连接标志 |
| `ble connect ADDRESS TYPE [--remember\|--temporary]` | 连接扫描到的可连接设备，默认成功后记住稳定身份 |
| `ble saved [SLOT]`、`ble use SLOT` | 查看/连接保存的身份，不要求先重新扫描 |
| `ble reconnect` | 连接最近一次保存的首选 BLE 身份 |
| `ble remember` | 保存当前主动连接的稳定身份 |
| `ble forget SLOT\|all` | 删除连接目标记忆，取消待保存请求 |
| `ble bonds [INDEX]` | 查询 NimBLE 配对身份及最近取消配对的结果 |
| `ble unpair ADDRESS TYPE`、`ble unpair all` | 删除指定/全部 BLE 配对密钥，终止对应现有链接 |
| `ble disconnect` | 取消/断开主动连接及待切换目标，保留入站 NUS 客户端和记忆 |
| `ble on`、`ble off` | 运行时启用/关闭 BLE 广播、扫描及链接 |

`SLOT` 固定为 0–3，删除一个槽不会让其他槽重新编号。`saved` 响应中的
`used_mask` 指示有效槽，例如 5 表示槽 0、2 有记录；`preferred=-1` 表示无首选。
每次查询读取一个槽；`count` 是有效槽数量，不代表最后一个槽的编号。
满四个槽时不会自动淘汰旧记录，需要先 `forget`。临时连接仍然可用。

`TYPE` 接受扫描结果中的数字，或 `public=0`、`random=1`、`public-id=2`、
`random-id=3`。请原样使用扫描的地址类型，不要猜测。示例：

```text
ble scan
ble results
ble results 1
ble connect AA:BB:CC:DD:EE:FF public
ble status
ble peer
ble saved
ble connect CA:11:22:33:44:55 random --temporary
ble status
ble remember
ble use 0
ble forget 0
ble bonds
ble unpair AA:BB:CC:DD:EE:FF public
```

### 记忆和重连规则

- 新的 `wifi.connect` 和屏幕连接操作在取得 DHCP 地址后保存。错误密码或连接
  超时不会覆盖旧的凭据。Wi-Fi 待保存期限为 120 秒；超时后仍可在连接成功时
  手动执行 `wifi remember`。`wifi.status.memory_error` 报告保存失败，
  `remember_pending` 报告是否仍在等待连接成功。
- `--temporary` 不新增或修改任何记忆，也不会删除同名的已有记忆。
  此后可显式 `remember`。成功保存的 Wi-Fi 是下次启动的首选，并沿用已有退避重试。
  删除首选后不会擅自从剩余记录中挑选其他网络；用 `wifi use SLOT` 指定新的首选。
- `forget` 保留当前活跃连接和本次运行中驱动需要的参数；它移除持久记忆。
  如果还需要立即停止连接，接着执行 `disconnect`。`wifi forget all` 同时取消
  尚未完成的保存请求，避免迟到的 DHCP 结果重新写入。`ble forget` 取消待保存的
  BLE 目标，已删除的槽不会因迟到的连接回调重新出现。
- BLE 连接成功后保存公共地址、静态随机地址或协议栈解析出的稳定身份；等待
  成功的期限为 30 秒。无法解析的 RPA/非可解析私有地址会变化，保存返回
  `ESP_ERR_NOT_SUPPORTED`，连接本身仍然可用。使用临时连接，或与能够提供稳定
  身份的设备完成配对后显式 `ble remember`。
- 保存 BLE 身份不等于建立安全配对；`ble forget` 管理主动连接目标，
  `ble unpair` 管理 NimBLE 密钥。需要彻底移除一个设备时，两者都执行。
  `unpair` 使用 `ble bonds` 返回的身份地址，可能断开正在使用 API 的 BLE 客户端。
  本地无法删除对端保存的密钥，必要时也需在对端取消配对。
- BLE 启动后恢复配对密钥并广播；**不会自动向保存的目标发起主动连接**，
  使用 `ble reconnect` / `ble use SLOT`。选新目标时先异步取消/断开旧主动链接，
  再建立新链接；断开阶段有 15 秒超时，不阻塞应用任务。
- ESP32-S3 的蓝牙功能为 BLE，不支持经典蓝牙 SPP/A2DP；主动连接继续沿用
  主服务发现功能，不会向陌生设备写任意 GATT 特征。

Wi-Fi 正在扫描时，连接切换可能返回驱动忙；等待 `scanning=0` 再提交。
BLE 未启用时先执行 `ble on` 并查询 `ble status`，再扫描/连接。
开关只影响本次运行，不改启动配置或删除记忆。

## USB 设备侧协议 v1

同一串口既可输入上述文本命令，也可直接输入现有 API v1 JSON。文本命令先编译
为 JSON，再调用共享处理器。物理串口来源由固件入口确定，不能通过请求字段伪造。
物理串口不要求 token；HTTP/BLE 修改操作仍要求已有 `token`。

```json
{"v":1,"id":10,"op":"wifi.connect","ssid":"Home","password":"password","remember":true}
{"v":1,"id":11,"op":"wifi.saved","index":0}
{"v":1,"id":12,"op":"wifi.use","slot":0}
{"v":1,"id":13,"op":"wifi.forget","slot":0}
{"v":1,"id":14,"op":"wifi.forget","all":true}
{"v":1,"id":15,"op":"ble.connect","address":"AA:BB:CC:DD:EE:FF","address_type":0,"remember":true}
{"v":1,"id":16,"op":"ble.unpair","address":"AA:BB:CC:DD:EE:FF","address_type":0}
```

输入是一行一个 UTF-8 JSON 对象，最多 256 字节；CLI 输入行最多 512 字节，
转换后的 JSON 同样必须在 256 字节内。行前可选 `RS=0x1e`。拒绝超长、无效 UTF-8、
嵌入 NUL、重复字段、尾随 JSON、过深嵌套及错误参数类型。修改命令拒绝未知字段；
`slot` 与 `all:true` 不能混用。输入损坏或超长时丢弃整行直到换行，
绝不把剩余后缀当成另一条命令。未完成输入空闲 30 秒后作废，到换行时报告错误。

输出使用 **RFC 7464 风格 JSON Text Sequence**：

```text
<0x1e>{"v":1,"id":10,"ok":true,"result":"accepted","ticket":27}<LF>
```

这是一个真正的 0x1e 控制字节，不是可见的 `<0x1e>` 文本。底层 VFS 可能把 LF
转换为 CRLF，接收端应接受两者。每条响应用一次 stdio 写入，与 IDF 日志共享输出锁；
保留启动和运行日志，上位机只解析 RS 开头、换行结束的 JSON 记录。
USB 暂停或重新枚举时可能丢失输出；等待下一条完整 RS 记录重新同步，
不要因为丢了响应就认定修改操作未执行。

`v` 固定为 1，`id` 为客户端提供的 0–2147483647 整数；CLI 自动生成 ID。
修改操作额外返回设备统一分配的 `ticket`，与不同传输客户端重复使用的 `id` 无关。
请求 ID 不提供去重或“恰好执行一次”保证。

```json
{"v":1,"id":20,"op":"command.result","ticket":27}
```

响应 `state` 为 `queued`、`applied` 或 `failed`，附 `error_code/error_name`。
`applied` 表示应用任务已调用驱动或提交到 NimBLE 队列，**不等于已连接或已保存**。
确认连接看 `wifi.status` / `ble.status` 的状态，确认保存看
`remember_pending=0`、`memory_error=0` 和 `saved` 中的目标记录。
取消配对需等 `ble.bonds.generation` 更新并检查 `error`；这一步在 NimBLE 任务执行。
最近 16 条已接收命令保留执行结果；更老或重启前的 ticket 返回 `unknown_ticket`。
四项队列已满时返回 `busy`，客户端应有界退避，避免无限快速重试。

Wi-Fi/BLE 结果最多各缓存 16 项，主服务 UUID 最多 8 项；读取扫描结果时应等待
完成，记住 `generation`，分页过程中若它变化则重新读取，避免混合两轮结果。
`capabilities` 返回当前传输的响应容量；BLE 特征值仍受 512 字节 ATT 上限约束。

兼容已有 API：`wifi.configure` 仍先保存再尝试连接，`wifi.clear` 清除全部 Wi-Fi
记忆并停用 STA；新客户端建议使用 `wifi.connect` 和明确的 `remember` 布尔值。
已有无线 `ble.connect` 若不传 `remember`，保持临时连接行为；本地 CLI 默认传 true。
原有 `status.last_id/last_error/completed` 保留，新的并发客户端应优先使用 ticket。

## 架构、存储和 USB 模式

`local_console_protocol` 是无堆分配的分帧/词法/命令翻译层，可在主机测试。
`SerialConsole` 是独立 IO 任务，网络回调和串口任务只查询快照或提交有界值类型请求。
`Service` 的应用任务串行执行驱动操作及 NVS 写入；BLE GAP/密钥操作继续在
NimBLE host 任务运行。UI 不承担串口轮询，也不等待终端输出。

Wi-Fi/BLE 目标各有 4 个槽，使用 `stick_net/radio_mem` 的固定长度版本化字节编码，
不把 C++ 结构体内存布局直接写入新记录。旧 `settings` 中的单个 Wi-Fi 自动迁移：
先提交新记录，再清理旧凭据副本，保留 AP 密码和 API token。中途复位可以重复迁移，
已存在的新记录优先；损坏或未知版本报告错误，不自动擦除其他 NVS 数据。
相同内容跳过写入。NVS 使用现有开发配置，未启用 flash/NVS 加密。

当前物理 USB 复用默认原生串口，不新增 USB 描述符，不改变下载/恢复方式。
芯片内部 PHY 同时只能被 USB Serial/JTAG 或 USB OTG 使用；进入已有 **USB DAP**
页面时串口任务暂停并丢弃半行输入，退出该页面后恢复，电脑可能需要重新打开串口。
如果切换前有未完成的输入，恢复后先发送换行清掉该无效行，再输入新命令。
Wi-Fi DAP / BLE DAP 不占用这个 USB 通道。启用
`M5_STICKS3_DAP_SMOKE_TEST` 的测试固件将 stdin 留给原有菜单测试入口，正式无线
控制台不启动，避免两个任务抢读。`CONFIG_M5_CONNECTIVITY_CONSOLE_ENABLED` 可关闭
控制台；关闭 Wi-Fi/BLE 不会禁用控制台，相关修改命令明确返回 `disabled`。

硬件核对使用用户提供的 `StickS3_Summary.pdf` 与
`K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf`；驱动行为参考
[Espressif USB Serial/JTAG 文档](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/api-guides/usb-serial-jtag-console.html)、
[ESP32-S3 技术参考手册](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)及
[NimBLE GAP unpair API](https://mynewt.apache.org/latest/network/ble_hs/ble_gap.html#c.ble_gap_unpair)，
最终 API 签名和行为以现有 Docker 内 ESP-IDF 5.5.4 源码核对。

## 构建与验证

```bash
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'source /opt/esp-idf/export.sh && idf.py -B build/radio-console build'
docker exec -w /workspace/esp32_idf/m5_sticks3 esp-dev bash -lc \
  'cmake -S tests -B build/radio-console-host -G Ninja -D M5_HOST_SANITIZERS=ON -D M5_HOST_IDF_PATH=/opt/esp-idf && cmake --build build/radio-console-host && ctest --test-dir build/radio-console-host --output-on-failure'
```

`radio_console_tests` 覆盖命令语法、UTF-8、CRLF、超长/损坏帧隔离、20 万随机输入字节、
存储编码/损坏拒绝、容量、固定槽及 ticket。`radio_service_tests` 直接编译生产
`Service` 与 SDK 自带 cJSON，只替换物理 IO/时钟/NVS，覆盖迁移、认证隔离、
成功后记忆、临时连接、保存错误、忘记与迟到事件、队列饱和及服务重启；
`radio_service_disabled_tests` 验证两个无线都未编译时的本地命令。
主机没有 IDF 源码时仍可运行纯协议测试；设置 `M5_HOST_IDF_PATH` 后启用服务集成测试。

### 实机验收（2026-09-22）

ESP-IDF 5.5.4 正式固件已烧录到 `/dev/ttyACM0` 对应的 StickS3，芯片为
ESP32-S3-PICO-1 v0.2，8 MB flash / 8 MB PSRAM。烧录前单独备份 NVS，未整片擦除。
12 项 CTest 再次全部通过（新增协议/服务测试启用 ASan/UBSan）。

| 实测项 | 结果与边界 |
| --- | --- |
| USB 命令与 JSON | 帮助/状态/记忆查询、UTF-8、CR/LF/CRLF、RS、退格及 17 类非法输入全部通过；长行后缀不会执行，30 秒半行超时后能恢复。 |
| 扫描与开关 | Wi-Fi/BLE 多轮真实扫描、最多 16 条分页、运行时关/开后重新扫描通过。关闭 BLE 后扫描失败以 host 异步诊断确认，而非将 `ticket=applied` 当作扫描成功。 |
| Wi-Fi 生命周期 | 用户临时热点 `kk-laptop` 成功取得 DHCP 地址；临时连接不保存、手动保存、默认成功后保存、复位自动重连通过。错误密码尝试不覆盖可用密码，`wifi use` 能恢复；forget 保留当前链接，忘记后再复位不自动连接。 |
| BLE 生命周期 | 主动连接用户电脑临时 BLE 广播，发现 3 个主服务；临时连接、稳定公共身份记忆、use/reconnect、已连接目标重新选择、复位恢复记忆和 forget 均通过。BLE 复位后保持不自动外连。此次只有一个目标，重选同一目标不等于两个外设间切换已验收。 |
| 压力与运行状态 | 空闲时 320 条、Wi-Fi 连网时 1000 条突发 JSON 请求全部响应。测试期间未检测到 panic、看门狗或意外复位；扫描期间持续收到 UI 帧率日志，未代替实体按键/UI 操作验收。 |
| 无线认证隔离 | HTTP 无线回归 42 项、旧 API 回归 27 项通过（无需读取重试）。BLE 复用既有加密 bond，通过 21 个修改操作的未授权拒绝及 `origin=serial` 伪造拒绝；USB 物理权限未泄漏给无线客户端。 |
| 数据保留 | 测试前两种连接目标记忆均为空；测试仅创建/忘记自己的记录。最终恢复空记忆和 AP-only 状态，原有电脑安全配对保留，临时电脑广播退出清理。 |

尚未完成的专项硬件验收：两组不同 Wi-Fi/两个 BLE 外设间切换、RPA 身份解析、
实际取消安全配对、USB 物理拔插/真正背压慢读、USB DAP 进入退出后的串口恢复。
本次没有执行 `ble unpair`，避免删除用户原有电脑配对。

### 复现硬件测试

关闭其他串口监视器；Python 串口脚本需要 `pyserial`。默认串口测试仅查询/拒绝
非法输入；以下显式选项增加扫描、空闲无线开关和压力测试，不清除记忆/配对：

```bash
python3 tests/radio_serial_test.py --port /dev/ttyACM0 \
  --scan-rounds 1 --power-cycle --stress-rounds 40 --idle-timeout
python3 tests/radio_api_test.py --url http://DEVICE_IP
python3 tests/connectivity_api_test.py --url http://DEVICE_IP
```

`radio_memory_hardware_test.py` 会建立连接、保存/忘记测试目标并复位整机，只允许
所测无线的记忆库初始为空，防止覆盖用户已有记录。仅对自己拥有的测试目标运行；
复位会中断其他链接。密码使用隐藏提示或 `M5_TEST_WIFI_PASSWORD`，不要写入脚本。
失败时保留现场并报告状态，不自动删除可能需要诊断的数据。

```bash
python3 tests/radio_memory_hardware_test.py --wifi-ssid YOUR_TEST_AP
```

Linux 电脑侧临时 BLE 广播需要系统 `python3-dbus` / `python3-gi`，使用
[BlueZ 广播接口](https://bluez.readthedocs.io/en/latest/advertising-api/)。它不更改
适配器电源、名称或配对，在退出或指定时间后注销。另一个终端让 StickS3 扫描，
从最新结果选择测试名称的**真实地址与类型**，再运行记忆测试，不要猜测地址类型：

```bash
python3 tests/ble_test_advertiser.py --name M5-Radio-Test-PC --seconds 600
# 在另一终端扫描并确认目标后：
python3 tests/radio_memory_hardware_test.py --ble-address TEST_ADDRESS --ble-type 0
```

已有电脑配对时，可运行 `python3 tests/radio_ble_auth_test.py --address DEVICE_BLE_ADDRESS`
验证加密链接与认证隔离。该脚本要求目标原本未连接，结束仅断开测试链接，不取消配对。
