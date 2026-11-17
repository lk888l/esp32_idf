# 本地网页与 100 Hz 实时通信

设备提供离线可用的响应式网页。网页、PC 客户端和 App 可共用 HTTP 控制与 WebSocket 数据协议；BLE/USB v1 接口继续保留。微信小程序需要单独的 BLE/局域网适配和真机验收，本版没有交付小程序、云端、MQTT 或 OTA。

## 使用

1. 设备已有 Wi-Fi 配置时，读取 SYSTEM 页上的 STA IP，在同一局域网浏览器中访问 `http://设备IP/`。
2. 在设备 WIFI → Local hotspot 打开恢复热点并显示六位 Web code。没有已保存网络的设备会在启动时开启热点。热点密码只在设备界面查看；连接热点后访问 `http://192.168.4.1/`。
3. 在网页输入 Web code。授权码两分钟有效，只能使用一次，五次错误后作废；K2 可生成新码，长按 K2 撤销全部网页会话并生成新码；K1 返回。
4. 授权后自动开启姿态曲线。暂停或关闭页面会释放该订阅的采集需求；本地 MOTION 等功能仍可继续采集。
5. 网页支持填写 SSID/密码并连接，设备取得 IP 后才记忆凭据。网络切换导致网页断开时，用屏幕显示的新 IP 重新打开。

最多两个临时会话、两个 WebSocket 客户端。会话在最后一次授权请求/订阅操作后 30 分钟过期；网页打开时的状态查询会续期。会话只保存在浏览器当前标签页的 sessionStorage，设备重启后失效。网页“退出并撤销授权”立即使对应数据通道失效。

本地 HTTP/WS **没有 TLS 保密性**，仅用于可信热点/局域网。不得将端口映射到公网。Origin 校验用于限制浏览器来源，不能替代认证或传输加密。远程应用需要另行实现 TLS、可信服务器和设备身份体系。

默认关闭启动日志中的密码和长期令牌；开发者确有需要时可以开启 `CONFIG_M5_CONNECTIVITY_LOG_SECRETS`。旧客户端的 NVS 令牌不改变。可信 USB 可发送 `{"v":1,"id":1,"op":"web.pair"}` 取得一次性码，`web.revoke` 撤销所有网页会话。两者均仅允许物理 USB，不能通过已授权网页、旧令牌或 BLE 提升权限。

## 接口

旧 `/api/v1/status`、`/api/v1/command` 和串口/BLE 请求仍使用原协议。新增接口如下：

| 接口 | 行为 |
| --- | --- |
| `GET /`、`GET /app.js` | 固件内置网页资源，无外部 CDN |
| `GET /api/v1/web` | 流版本、字段顺序、单位、限制、启动标识 |
| `POST /api/v1/session` | `{"code":123456}` 换取短期 `session`；`{"op":"revoke"}` 撤销当前会话 |
| `POST /api/v1/control` | 现有 v1 请求体，认证改用 `Authorization: Bearer SESSION` |
| `GET /api/v1/stream` | WebSocket 升级，必须提供 `Origin: http://设备IP`，包括原生客户端 |

POST 要求 `Content-Type: application/json`。控制请求仍不超过 256 字节；响应不超过 767 字节。网页只支持设备 IP 同源访问，不开放通配 CORS。WebSocket 的来源与容量检查发生在握手前；未授权连接十秒后关闭。

新控制接口必须携带递增的 `X-Command-Seq`（1..4294967295）。同一会话、同一序号、完全相同的请求字节返回首次结果；改动请求则返回 HTTP 409 `sequence_conflict`。最近 32 个请求的响应保留在内存，旧序号过期后返回 409 `sequence_expired`，不会重新执行。传输重试必须保留序号和请求字节，不能以新序号重发不确定是否成功的修改。序号用尽应建立新会话。

`accepted + ticket` 只表示入队；`command.result` 的 `applied` 表示驱动调用完成。Wi-Fi 业务成功还需要确认取得 IP 和记忆结果。原有 ticket 历史仍为 16 次提交，与会话响应缓存不同。

WebSocket 首帧：

```json
{"op":"subscribe","session":"短期会话令牌"}
```

返回 `subscribed` 事件及启动标识；使用相同结构的 `unsubscribe` 取消订阅。每个消息必须是完整、非分片的文本帧，入站最多 256 字节。服务端支持 Ping/Pong。

实时帧示例（`samples` 各列依次为序号、采样时间估计 µs、roll/pitch/yaw 度数、有效性）：

```json
{"v":1,"event":"motion","boot":123,"lost":0,"valid":true,"samples":[[1,10000,0.1,0.2,0.3,true]]}
```

- 底层 BMI270/VQF 保持 200 Hz，每两个实际融合样本输出一次，目标 100 Hz；不通过定时轮询最新快照伪造样本。
- `time_us` 是以每次 FIFO 读取的设备单调时钟为锚点、按 200 Hz 周期重建并保持递增的采样时间估计，不是精确硬件时间戳或 UTC。传感器重启/异常恢复可能形成时间缺口；`boot` 变化表示设备服务重启。
- 约每 50 ms 收集一批，每帧最多五个样本。调度抖动可能产生连续多个小批次。网页绘图 25 FPS，与采样频率独立。
- 每客户端最多四个待发送批次，另有一个正在发送的帧；溢出丢最旧遥测，`lost` 累计报告。序号同时支持检测丢失；断线重连从最新样本开始。
- TCP 发送使用非阻塞调用并保存部分发送进度，不在采集任务等待网络。持续两秒没有发送进展的连接会关闭。控制与遥测缓冲独立。
- 每秒 `stream.status` 事件报告传感器有效性和内部堆最低值。没有新样本时，客户端必须显示过期状态，不能把最后一个姿态当成实时值。
- 本版提供实时观察，**不保证无损记录**。BLE 仍是原有查询/配网通道，没有承诺 BLE 同样达到 100 Hz。

## 生命周期与恢复

采集模块分别处理本地页面需求和网络订阅需求；只在所有需求释放后停止。模型层的固定环形缓冲保护采集任务，不引入模型→联网组件依赖。Wi-Fi/HTTP、会话/流、命令执行保持独立职责。

有凭据时启动 STA；无凭据时启动受密码保护的 APSTA。实体打开 Local hotspot 或可信命令 `wifi.hotspot` 可开启恢复热点。热点开启至少两分钟，STA 已联网且没有 AP 客户端时自动关闭；不会主动踢掉正在配网的客户端。网络重连在原有指数退避基础上增加 ±12.5% 随机抖动，原始断线原因继续通过状态输出。

网页授权缓存与发送缓冲分配在 PSRAM；短时临界区只用于模型快照和样本复制。HTTP 服务停止并回收连接后才释放 WebService。量产 Flash/NVS 加密、安全启动、OTA 分区与回滚属于后续独立工作，本次不改变分区、不烧写 eFuse。

## 验证

主机测试：

```sh
cmake -S tests -B build/network-host -G Ninja -DM5_HOST_SANITIZERS=ON -DM5_HOST_IDF_PATH=/opt/esp-idf
cmake --build build/network-host
ctest --test-dir build/network-host --output-on-failure
```

实机协议测试（需要 pyserial、websockets）：

```sh
python3 tests/web_stream_hardware_test.py --port /dev/ttyACM0 --duration 30 --output /tmp/sticks3-web-results.json
```

此测试通过物理 USB 建立临时授权，验证拒绝未授权访问、重复命令、序号冲突/过期、双客户端、慢接收端、重新订阅和撤销。它不改变 Wi-Fi 凭据，但会撤销已有网页会话；运行时保持设备不在 MOTION/游戏页面，以验证最后一个网络订阅退出后的传感器释放。

数据到达间隔不能直接证明端到端显示延迟；200 ms P95 显示延迟目标、24 小时长稳和 Android/iOS 真机兼容性需分别记录测量结果，不能以主机测试或桌面移动尺寸模拟代替。

浏览器交互与长期测试：

```sh
python3 tests/web_browser_test.py --chromium /usr/bin/google-chrome
python3 tests/web_stream_soak.py --duration 86400 --output /tmp/sticks3-soak.json
```

浏览器测试使用 Playwright，覆盖 Chromium 桌面和手机尺寸视口。长期测试每分钟写入采样、缺口、堆与在线时长，使用一个客户端，保留另一个客户端名额；USB 仅用于开始时授权。只有结果中 `state: passed` 才表示完成指定时长，`running` 不是验收通过。

### 本次验收记录（2026-11-17）

使用 Docker `esp-dev` / ESP-IDF 5.5.4，在 `/dev/ttyACM0` 对应的 ESP32-S3-PICO-1（8 MB Flash / 8 MB PSRAM）上验证。原始完整 Flash 与独立 NVS 副本保存在本机受限目录 `build/connectivity-hardware/`，未变更分区。

- 正常固件与 Wi-Fi/BLE 均关闭的裁剪固件构建通过；正常版本已烧录。
- 启用 ASan/UBSan 配置的主机测试 22/22 通过；原 HTTP 27 项、无线诊断 HTTP 42 项检查通过。
- USB 8 组回归通过，包括 BLE 扫描、关闭/重开，保存的配置和 bond 数量保持不变。
- 双客户端 30 秒窗口各约 99–100 Hz，无样本序号缺口；支持空 Ping/Pong、标准关闭握手、撤销后断开和最后订阅释放采集。
- Chromium 桌面和手机尺寸视口的授权、曲线、暂停/恢复、刷新、模拟离线恢复和撤销均通过，无 JS 错误。截图与 JSON 结果见 `build/network-results/`。
- Wi-Fi 关闭再启用后约 6.11 秒恢复连接；无 AP 客户端时恢复热点约 119.92 秒后关闭，STA 保持联网。
- 24 小时长稳以 `build/network-results/soak.json` 实际状态为准；Android Chrome、iOS Safari、微信小程序真机以及端到端显示延迟 P95 <200 ms 尚未完成验收。不能把手机尺寸视口或帧到达间隔当成这些项目已通过。
