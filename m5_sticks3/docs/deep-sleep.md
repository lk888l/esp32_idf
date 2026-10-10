# L2 深度休眠

主菜单第 15 张 **SLEEP** 卡片用于让 ESP32-S3 深度休眠，同时保持 L2 电源和 RTC
计时。KEY2 打开卡片，短按 KEY2 选择唤醒方式，长按 KEY2 后松开开始 3 秒倒计时。
倒计时期间 KEY1 取消；未倒计时时 KEY1 返回菜单。

| 模式 | 唤醒方式 |
| --- | --- |
| BUTTONS | KEY1 或 KEY2，默认模式 |
| 5 MIN | 5 分钟后自动唤醒，也可提前按 KEY1/KEY2 |
| 30 MIN | 30 分钟后自动唤醒，也可提前按 KEY1/KEY2 |
| 1 HOUR | 1 小时后自动唤醒，也可提前按 KEY1/KEY2 |

进入休眠后屏幕、Wi-Fi、BLE、音频和 5V 升压关闭，IMU 进入低功耗挂起。
充电功能保持原来的设置。
唤醒会重新启动应用并返回主菜单，随后按照现有配置重新连接 Wi-Fi。
USB Serial/JTAG 在深度休眠期间断开，唤醒后重新枚举。

如果已经联网校时，休眠期间 RTC 继续计时，唤醒后即使未联网也能立即显示时间。
时钟组件保留校时状态、上次校时时间和时间基准的校验信息；唤醒重新联网后自动校时。
尚未校时也可以休眠，但唤醒后仍显示时间占位符，直到首次校时成功。
当前 `CONFIG_RTC_CLK_SRC_INT_RC` 使用内部低频 RC 时钟。RTC 保留保证时间延续，
不能保证休眠期间准确走时：进入休眠后芯片降温，RC 频率变化可能让时钟走快，
也会让同源的定时唤醒提前。IDF 已在休眠前自动同步系统计时器并校准 RTC，
应用不应再手动累加休眠时长；睡着后的温漂仍需在唤醒后通过联网校时纠正。

硬件没有独立 RTC 或 32.768 kHz 晶体，GPIO15/16 的晶体接口已用于音频。
`CONFIG_RTC_CLK_SRC_INT_8MD256` 可选择更稳定的内部 17.5 MHz 时钟源，
使用固定的 256 分频和 IDF 自动校准，不需要手工微调系数；官方给出的额外深睡
电流约为 5 µA。它仍是内部振荡器，不能承诺长期离线走时精度。正式配置保持原样。
参见 [ESP-IDF 系统时间说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/system_time.html)。

2026-10-09，当前内部低频 RC 配置的 300 秒定时实测：入睡日志到时钟恢复日志的
主机实耗 296.81 秒，RTC 前进 301 秒，相对走快约 4.19 秒；定时唤醒原因和联网前
校时状态恢复均正确。临时切换到 `INT_8MD256` 的同设备对照中，入睡到唤醒日志
的实耗为 300.84 秒，RTC 前进 301 秒，但随后 BSP 初始化回滚失败，未完成时钟
与界面恢复验证。因此没有采用该配置，测试后恢复原参数正式固件。

使用 **KEY1/KEY2** 唤醒。机身独立电源键仍遵循 PM1 的复位/关机行为；双击关机、
电池耗尽或完全断电会切断 L2，无法保持 ESP32 RTC。冷启动和普通复位均要求重新
联网校时，避免显示未经验证的时间。

## 电源与唤醒实现

`components/sleep` 接收 UI 请求，主任务按反序停止所有模块，确保外设不再访问
电源和总线。随后隔离 LCD/音频信号引脚，防止通过信号线向已关闭的 L3B 供电。
PM1 GPIO2/GPIO3 分别关闭 LCD/音频供电和功放，`PWR_CFG` 保留 bit1 的 ESP32
3.3V 电源，保留 bit2 的 IMU 接口供电，关闭 bit3 的升压和 bit4 的指示灯，同时
保留 bit0 的充电设置。BMI270 的传感器关闭并启用 advanced power save；其 I2C
直接连接带 L2 上拉的共享总线，所以保留 L1 可防止接口反向供电。
不发送 PM1 关机命令。

进入休眠前清除可能残留的 PM1 看门狗和倒计时动作，读取寄存器确认 L2 供电状态，
再开启 PM1 的 2 秒 I2C 空闲休眠。KEY1 GPIO11 和 KEY2 GPIO12 使用 RTC ext1
任一低电平唤醒，原理图中的外部上拉由 L2 供电。可选定时器使用 ESP32 RTC。
RTC slow memory 保持供电以保存时钟元数据。

唤醒时释放 GPIO 保持及 RTC 引脚配置，BSP 恢复 IMU 和正常电源配置，再初始化
屏幕、网络及其他模块。电源准备失败时取消休眠；模块已停止后的错误通过复位恢复，
避免留下部分运行的应用。

依据：[StickS3 M5PM1 电源说明](https://docs.m5stack.com/zh_CN/arduino/m5sticks3/m5pm1)、
用户提供的 K150 V0.6 原理图及
[ESP-IDF ESP32-S3 休眠说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/sleep_modes.html)。

## 复现验证

正常固件构建、烧录与项目现有流程相同。自动实机验证使用单独的 DAP smoke 固件，
串口测试命令只存在于该测试构建：

```bash
source /opt/esp-idf/export.sh
idf.py -B build/clock-menu-smoke -D SDKCONFIG=build/clock-menu.sdkconfig \
  -D M5_STICKS3_DAP_SMOKE_TEST=ON build
idf.py -B build/clock-menu-smoke -p /dev/ttyACM0 flash
python3 tests/deep_sleep_hardware_test.py --port /dev/ttyACM0
```

测试通过实际 SLEEP 页面倒计时触发 5 秒休眠，处理 USB 断开与重新连接，检查
RTC 定时唤醒、休眠前后时间延续、重复休眠和唤醒后的菜单导航。按键电气唤醒需要
在设备上实际按 KEY1/KEY2 验证。主机测试覆盖电源寄存器、错误恢复、时钟恢复及
界面倒计时/取消操作。验证后重新烧录正常固件。

默认的 5 秒测试只能检查计时延续。检查较长休眠的走时偏差时，可执行：

```bash
python3 tests/deep_sleep_hardware_test.py --port /dev/ttyACM0 \
  --cycles 1 --sleep-seconds 300
```

也支持 1800、3600 秒；长时测试使用现有页面的正常定时模式。`MEASURE` 输出主机
单调时钟实耗、联网校时前的 RTC 增量和两者差值 `gain`，正值表示走快。
`--max-drift-seconds` 默认为 3 秒，是测试允许误差，不是设备校准系数。
计时使用整数秒和日志接收时间，包含启动与 USB 重枚举延迟，不能用于微秒级测量。
串口重连按官方监控器的 DTR/RTS 顺序操作，避免 USB 控制线造成额外复位；
联网校时计数增加后允许时间向后修正，避免把正确纠偏误判为恢复失败。
