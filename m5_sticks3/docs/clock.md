# 时钟与联网校时

主界面左上角显示 `HH:MM:SS`，沿用原标题的字号与位置，每秒刷新。未校时前
显示 `--:--:--`，联网校时后自动显示当前时间。

主菜单第 14 项 **CLOCK** 显示 24 小时制时间、日期、星期、时区、校时状态和距上次
校时的时间。默认是北京时间 UTC+8。KEY1 返回菜单，短按 KEY2 请求校时，长按
KEY2 返回菜单。第一次成功校时前显示占位符，避免把 1970 年或未验证的系统时间
当作当前日期。

设备使用已有 Wi-Fi STA 配置，不单独保存网络密码。先在 WIFI 页面或 USB 串口完成
配网；只开启设备备用热点不会获得互联网时间。取得 STA 地址后自动校时，断网时
系统时钟继续运行，重新联网时再次校时。正常固件每小时刷新一次。

校时参考了 `espc3-integrated/components/network/src/network.cpp` 的延后启动 SNTP
和时区设置方式，使用 ESP-IDF 的
[esp-netif SNTP 接口](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/network/esp_netif_programming.html#sntp-service)。
独立 `components/clock` 组件负责时钟，由现有 connectivity 服务管理生命周期。
UI 与 USB/HTTP/BLE 从线程安全快照读取时间。网络请求、DNS、备用服务器选择和周期
更新由 lwIP 后台处理，不等待网络响应或阻塞 LVGL。

主服务器默认 `ntp.aliyun.com`，备用 `pool.ntp.org`。一分钟内未收到响应会显示
`NTP RETRYING`；底层继续退避重试和切换服务器，收到响应后自动恢复。已校准时间
在重试期间继续显示。手动请求会合并，重复重启有 10 秒间隔，避免连续按键打断
服务器切换。`NTP SYNCED` 代表收到过有效响应；`sync_age_s` 可判断时间的新鲜程度。

硬件资料（StickS3 Summary 及 K150 V0.6 原理图）未列出独立电池备份 RTC。
本实现使用 ESP32 系统计时；通过 **SLEEP** 卡片进入 L2 深度休眠时保留 RTC 供电，
并在 RTC 内存保存校时元数据。深度休眠唤醒后可离线显示延续的时间，并在重新联网
后自动校时。普通复位、关机和断电后需重新联网成功校时再显示时间，详见
[深度休眠说明](deep-sleep.md)。

## USB 与统一 API

```text
clock
clock status
clock sync
ntp
help clock
```

HTTP/BLE/USB JSON 均支持 `{"v":1,"id":1,"op":"clock.status"}`，返回 `state`、
`valid`、`online`、`synced`、`time`、`date`、`weekday`、`timezone`、`zone`、
`epoch`、`last_sync_epoch`、`sync_age_s`、`sync_count`、`interval_s` 和 `error`。
`sync_age_s=-1` 表示尚无有效校时基准；深度休眠唤醒后的值包含休眠时间。
`epoch` 为 UTC Unix 秒数，显示字符串
已转换为配置的时区。时间无效时 `epoch=0`，显示字符串为占位符。

手动校时使用 `{"v":1,"id":2,"op":"clock.sync"}`。无线请求需要现有 API token，
USB 沿用物理连接鉴权。成功返回 `result:"requested"`，表示已经提交校时请求；
它没有无线命令的 `ticket`。轮询 `clock.status`，确认 `sync_count` 增加及状态恢复
`synced`。无 STA 网络返回 `wifi_not_connected`，冷却期返回 `busy`。

## 构建配置

`idf.py menuconfig` 的 **M5-StickS3 Clock** 可设置：

| 配置 | 默认值 | 用途 |
| --- | --- | --- |
| `M5_CLOCK_TIMEZONE` | `CST-8` | POSIX TZ 字符串；符号与 UTC 偏移相反 |
| `M5_CLOCK_TIMEZONE_LABEL` | `UTC+8` | 页面显示标签 |
| `M5_CLOCK_NTP_SERVER` | `ntp.aliyun.com` | 主服务器 |
| `M5_CLOCK_NTP_SERVER_BACKUP` | `pool.ntp.org` | 备用服务器 |

`LWIP_SNTP_MAX_SERVERS` 至少为 2，`LWIP_SNTP_UPDATE_DELAY` 默认 3,600,000 ms。
页面使用 Montserrat 36 字体，已经同时启用正式 `sdkconfig` 和默认配置。

## 验证

在 ESP-IDF 容器内构建正常固件与运行主机测试：

```bash
source /opt/esp-idf/export.sh
idf.py -B build/clock-idf -D M5_STICKS3_DAP_SMOKE_TEST=OFF \
  -D M5_STICKS3_HW_SMOKE_TEST=OFF -D M5_STICKS3_RADIO_SMOKE_TEST=OFF build
idf.py -B build/clock-idf -p /dev/ttyACM0 flash
cmake -S tests -B build/clock-host-tests -G Ninja -D M5_HOST_IDF_PATH=/opt/esp-idf
cmake --build build/clock-host-tests
ctest --test-dir build/clock-host-tests --output-on-failure
cmake -S tests/ui -B build/clock-ui-tests -G Ninja
cmake --build build/clock-ui-tests
ctest --test-dir build/clock-ui-tests --output-on-failure
```

主机时钟测试覆盖 UTC+8 午夜换日、未校时占位符、断网计时、重连、手动请求合并、
超时及启动失败恢复。UI 测试使用真实 LVGL 渲染各状态，检查控件边界及文字裁切，
预览位于 `build/clock-ui-tests/previews/clock_*.ppm`。

正常固件且已联网时，在有 pyserial 的宿主机运行：

```bash
python3 tests/clock_hardware_test.py --port /dev/ttyACM0
python3 tests/clock_hardware_test.py --port /dev/ttyACM0 --exercise-wifi
```

默认只读；`--exercise-wifi` 会短暂关闭并恢复 Wi-Fi，验证离线走时、重连校时和
手动校时，并检查保存网络的元数据未变化。执行期间设备的无线连接会暂时中断。
脚本预期默认 UTC+8/一小时配置，主机本身应已校准时间。

2026-10-09，ESP-IDF v5.5.4 容器正式构建通过，16 项主机测试、3 项 LVGL 界面测试
通过。StickS3 实机自动校时、与主机时间比对、断网走时、断网请求拒绝、重连自动
校时及手动校时通过，现有 Wi-Fi 记忆保持不变。

独立菜单测试固件使用 `sticks3-clock-test.invalid` 主服务器、`ntp.aliyun.com`
备用服务器和 60 秒刷新周期，实机验证备用校时及自动周期刷新；CLOCK 页面 KEY2
也成功触发新一轮校时。菜单验证通过 14 次普通切换、8 次快速切换和
AURA/SYSTEM/CLOCK 页面往返，LVGL 分配峰值 94,544 字节。测试结束恢复了
主服务器 `ntp.aliyun.com`、备用 `pool.ntp.org`、3,600 秒周期并关闭所有测试入口。
每小时周期由相同 SNTP 定时逻辑实现，本轮未持续等待完整一小时。
