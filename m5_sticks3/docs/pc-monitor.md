# Wi-Fi 电脑资源监控

实现参考本地最新提交：`espc3-integrated ad1ebef`、`wl1-studio b969bf7`。
电脑采集本机指标，经 UDP 4443 发送给 StickS3；固件与现有上位机协议兼容。

## 使用

1. 电脑与 StickS3 连接同一局域网，电脑也可使用有线网络。在 WIFI / SYSTEM 页
   或 USB `wifi status` 查看设备 IP。电脑连接设备热点时使用 `192.168.4.1`。
2. 启动 wl1-studio 桌面应用，在 **ESP32-C3 像素桌宠 → 电脑监控** 填写
   **StickS3 的 IP**。当前上位机入口名称仍为 C3，同一协议可直接用于 S3。
3. 选择 500 / 1000 / 2000 ms 更新周期，按需开启 NVIDIA GPU 采集，点击开始。
   上位机“设备已响应”表示收到本 S3 返回的会话 ACK。
4. 设备主菜单第 16 张 **PC MON** 卡片按 KEY2 打开。KEY2 切换总览/详情，
   KEY1 或长按 KEY2 返回。底部显示当前 STA IP；无 STA 连接时显示热点 IP。

总览显示 CPU / RAM / GPU 百分比与进度条；详情显示内存与显存已用/总量、GPU 温度。
容量使用 MiB/GiB/TiB 等二进制单位，名称过长时单行省略。
首个 CPU 样本建立采样基线，可能为 N/A；关闭 GPU 或驱动不可用时 GPU 项为 N/A，
CPU / RAM 继续更新。超过 3 秒无新样本提示 `STALE / RETRY`，过期数值显示 N/A。

离开 PC MON 页仍继续接收。上位机原有轻量后台与托盘传输可直接使用；电脑仍需运行
采集核心，设备仍需供电。只支持单台电脑向一个设备单播，目标 IPv4 手动填写；
DHCP 重新分配 IP 后需更新目标地址。

不启动完整桌面界面也可运行原始采集核心，在 wl1-studio 目录执行：

```bash
cargo run --manifest-path src-tauri/Cargo.toml -p pc-monitor-core --example send -- 设备IP 30
cargo run --manifest-path src-tauri/Cargo.toml -p pc-monitor-core --example send -- 设备IP 30 --no-gpu
```

## 固件结构与诊断

- `components/pc_monitor`：固定大小解析器、最新值状态和非阻塞 UDP 服务。
- `connectivity::Service` 在现有 20 ms 应用循环里处理最多 4 包，无额外接收任务。
  界面仅每 250 ms 读取快照，LVGL 任务独占显示。
- Wi-Fi 关闭时关闭 socket 并清除样本；重新启用后自动监听，socket 故障每秒重试。
  应用停止/深度休眠时先释放接收 socket，再停止无线设施。样本不写 NVS。
- API、UI 和接收端通过短临界区复制固定快照，读取时动态计算年龄。

USB CLI 与现有 API v1 共用以下只读查询，HTTP / BLE 同样可调用：

| 命令 | API op | 内容 |
| --- | --- | --- |
| `pc` / `pc status` | `pc.status` | port、listening、received、stale、age_ms、accepted、rejected、out_of_order、error、session、seq |
| `pc sample` | `pc.sample` | host、gpu_name、stale、CPU / RAM / GPU / VRAM / GPU 温度；不可用或过期的数值为 null |
| `help pc` | `help` + `topic: "pc"` | 配置与操作提示 |
| `capabilities` | `capabilities` | `pc_monitor: 1`、`pc_monitor_port: 4443` |

例如 USB 输入 `{"v":1,"id":1,"op":"pc.status"}`。`error` 为最近一次 socket errno，
正常为零；`accepted` 是接收样本数，ACK 发送失败会保留已接收样本并报告错误。

## UDP v1

完全沿用参考 C3 / wl1-studio 的 UDP **4443** 单播协议，UTF-8 JSON 严格小于
1024 字节，不占用调试器 TCP 4441 / UDP 4442。发送完整样本，不补发历史。

```json
{"v":1,"session":"0123456789abcdef","seq":1,"host":"MY-PC","cpu_pct":23.5,"ram_used":8589934592,"ram_total":34359738368,"gpu_name":"NVIDIA GPU","gpu_pct":42.0,"vram_used":4294967296,"vram_total":12884901888,"gpu_temp_c":55.0}
```

全部 12 个字段必须存在，未知/重复字段、非法 UTF-8、控制字符、无效范围及超长包
均拒绝。session 为 1–32 字节 ASCII 字母、数字、下划线或连字符；主机名称最多
64 字节、显卡名称最多 96 字节。seq 为 u32，同会话旧包/重复包不更新年龄也不回复，
支持回绕；新会话接受任意初始序号。

CPU/GPU 为 0–100 或 null；GPU 温度为 -100–250 °C 或 null；RAM/VRAM 为整数字节数，
总量必须大于零，已用不能大于总量；显存两个字段必须同时有效或同时为 null。
有效样本回复发送者：

```json
{"v":1,"session":"0123456789abcdef","seq":1,"ack":true}
```

此版本沿用参考工程的局域网监控模型，UDP 数据与 ACK 不含认证或加密。
上位机会校验目标、会话与已发送序号，3 秒无有效 ACK 后标记连接过期。

## 构建与验证

以下固件与测试命令在已加载 `/opt/esp-idf/export.sh` 的 `esp-dev` 容器内工程目录执行。
本轮复用已有 `build/clock-idf` 正式构建目录；可换成独立新目录。

```bash
idf.py -B build/clock-idf -D M5_STICKS3_DAP_SMOKE_TEST=OFF \
  -D M5_STICKS3_HW_SMOKE_TEST=OFF -D M5_STICKS3_RADIO_SMOKE_TEST=OFF build
idf.py -B build/clock-idf -p /dev/ttyACM0 flash
cmake -S tests -B build/pc-monitor-host-tests -G Ninja -D M5_HOST_SANITIZERS=ON
cmake --build build/pc-monitor-host-tests
ctest --test-dir build/pc-monitor-host-tests --output-on-failure
cmake -S tests/ui -B build/clock-ui-tests -G Ninja
cmake --build build/clock-ui-tests
ctest --test-dir build/clock-ui-tests --output-on-failure
```

具有串口与局域网访问能力、安装 pyserial 的主机可运行实板专项验收；运行前停止
其他向本板发送的监控会话。`--exercise-wifi` 临时开关 Wi-Fi，并在异常时尝试恢复：

```bash
python3 tests/pc_monitor_hardware_test.py --port /dev/ttyACM0 --exercise-wifi --duration 20
```

结果写入 `build/hardware/pc-monitor/result.json`。主机测试覆盖严格解析、乱序/回绕、
真实 loopback UDP、ACK、4 包处理上限、端口占用重试和释放；UI 测试输出
`build/clock-ui-tests/previews/pc_*.ppm`，包含无 Wi-Fi、等待、实时、过期、GPU 关闭和
长名称/容量边界。菜单实板回归沿用 README 的独立测试固件，现覆盖 16 张卡片及
PC MON 的打开、翻页与返回；测试后重新烧录正式固件。

## 本轮验收（2026-10-21）

ESP-IDF 5.5 Docker `esp-dev`、StickS3 实板 `/dev/ttyACM0`，沿用板上已保存的 STA 网络。
本轮未修改两个参考工程。

| 项目 | 结果 |
| --- | --- |
| 正式固件构建、烧录 | 通过；所有 smoke 入口关闭 |
| 主机回归 | 18 项通过，新增协议/UDP 服务及相关服务测试开启 ASan/UBSan |
| LVGL 135×240 回归 | 5 项通过，监控 8 种状态/边界截图已检查 |
| 实板 UDP 专项 | ACK、非法/超长/重复/乱序拒绝、3 秒过期、null 字段、新会话及 u32 回绕、Wi-Fi 关闭重连、20 秒持续发送通过 |
| 原始上位机核心 | 本机 CPU、RAM、RTX 5070 Ti Laptop GPU / VRAM / 温度，发送及会话 ACK 通过 |
| 实板 UI 与连续发送 | 32 次普通菜单切换、32 次快速请求，以及 AURA/SYSTEM/CLOCK/SLEEP/PC MON 往返通过；监控翻页/返回期间 seq 134→141→147 持续增长 |
| LVGL 内存池 | 菜单回归峰值 97,008 字节，最小采样连续空闲块 204,960 字节 |
| 深度休眠回归 | 5 秒定时唤醒 1 次，RTC 在 SNTP 前前进 6 秒（宿主机测得 5.92 秒），菜单及运动页面恢复 |
| Wi-Fi DAP 共存 | 监控持续接收时 UDP 4442 发现与 TCP 4441 DAP_Info 查询通过 |

实板记录位于 `build/hardware/pc-monitor/`，主机日志为 `build/pc-monitor-*-test.log`。
本轮无线链路使用 STA；AP 接入没有单独实测。DAP 共存检查覆盖端口与协议查询，
没有连接外部待烧录芯片。
