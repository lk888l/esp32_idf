# 音频与红外：菜单、接口及验证

本次新增功能面向 StickS3（ESP32-S3 / ES8311 / AW8737 / M5PM1），沿用工程的
AppModule 生命周期、LVGL 菜单和服务任务分层。2026-09-09 的验证在 Docker 和主机测试中完成，
**本轮没有连接设备或烧录，音质、声学性能、红外距离及真实电源切换仍需实机验收**。

## 菜单操作

主菜单现在包含原有七个功能与新增的 SPEAKER、MICROPHONE、INFRARED。
KEY1 切换动作，KEY2 执行；长按 KEY2 返回上一级。在功能主页返回主菜单会停止对应音频或红外活动。
进入麦克风页面不会自动开始采集，须执行 Start meter 或 Record clip。

| 页面 | 功能 |
| --- | --- |
| SPEAKER | 音调播放/停止、20–4000 Hz 频率、0.1–30 s 时长、音量、播放麦克风录音、采样格式、诊断 |
| MICROPHONE | RMS/峰值电平、连续电平表、0.1–30 s PSRAM 录音、停止/回放/清除、0–42 dB 增益、采样格式、诊断 |
| INFRARED | 连续接收、停止收发、学习、最近信号回放、四个持久存储槽、NEC 发送器、诊断 |
| NEC sender | 8/16 位地址、命令、0–20 次 repeat、20–60 kHz 载波、1–50% 占空比 |
| Saved slots | 选择 Slot 1–4，回放、学习、保存最近接收、删除 |
| Audio format | 8000 / 16000 / 24000 / 32000 / 44100 / 48000 Hz；16 / 24 bit，单声道 |

数值编辑器支持 Decrease、Increase、Step、Apply、Cancel。NEC 地址与命令用十六进制编辑；
修改格式会清除旧录音，界面会先提示。覆盖已保存的红外槽、删除槽、清除录音也有确认页面。
按键按下到释放间若数据对象发生变化，本次动作会被拒绝，避免作用于已变更的目标。

操作返回“已排队”表示服务已接受请求，实际完成或错误以页面状态为准。忙碌时不可执行的动作置灰，
停止命令不依赖普通命令队列空位。跨功能切换若硬件尚未停止，页面会报告忙碌，可在停止完成后重试。

## 音频行为与边界

I2S 使用 ESP-IDF 新 channel API，一次成对分配 TX/RX，在同一控制器共享 MCLK/BCLK/LRCK。
两方向都使用 DMA，六个描述符、每描述符 192 帧；DMA 驱动缓冲位于内部内存，录音放在 PSRAM。
16 bit 使用两字节 PCM；24 bit 使用四字节有符号容器，高 24 位有效、低 8 位补零。
ES8311 用 32 位 I2S 时隙传输完整 24 位采样，MCLK 为采样率的 256 倍。

默认 16 kHz / 16 bit、音量 30%、麦克风增益 24 dB。音调有淡入淡出；录音与电平表丢弃开始的
50 ms 稳定期；正常播放完成会等待 DMA 尾部排空，主动停止可中断等待。
RMS/峰值为 dBFS 数字电平，不是校准后的 dB SPL。诊断页 RX overrun 来自接收队列溢出回调；
TX underrun 是短写/写超时统计，不代表硬件独立检测到了欠载。

根据 PMIC 电源读数，电池供电或电源读取失败时实际音量最多 70%，外部供电时最多 100%；
播放过程中每秒复查。界面显示请求音量及限制后的实际音量。音调必须低于当前采样率的一半。

录音保留到清除、格式切换、服务关闭或重新录音；返回主菜单后可从 SPEAKER 回放。
录音和音频设置只在本次运行期间保留；红外存储槽写入 NVS，可跨重启保存。
30 s / 48 kHz / 24 bit 录音需要约 5.76 MB PSRAM。已有录音和其他模块占用会影响可用空间；
无法分配时报告错误，不退回内部 DMA 内存，也不重启设备。没有文件系统录音、MP3 解码或回声消除的隐含实现。

可复用音频接入使用 `audio::Service::start_stream(StreamCallbacks)`，支持 PCM source、
microphone sink 或两者并行，适合连接应用的有界缓冲区。回调在音频任务执行，不能等待网络或磁盘。
释放回调上下文前调用 stop，并等待单次快照中
`settled_generation == stop_generation` 且处于 Idle/Fault；成功 deinitialize 也保证任务已退出。
完整格式和所有权契约见 [audio 组件说明](../components/audio/README.md)。

## 红外行为与边界

TX/RX 都使用 ESP32-S3 RMT DMA，不使用 GPIO 中断采样或软件延时发射。分辨率为 1 MHz，
默认发射载波 38 kHz / 33%，RX 输入是解调包络。纯 C++ 协议层支持 NEC、扩展 NEC、
repeat 上下文，以及未知协议的原始脉冲保存和回放。

学习默认等待 30 s，一次成功捕获后停止接收并写入选定 NVS 槽。
repeat 只能关联 160 ms 内的有效完整 NEC 帧；孤立 repeat 不会覆盖可回放的完整信号。
存储格式明确规定版本、长度、小端序和 CRC，读取时检查容量、脉宽、极性交替和总时长，
不直接序列化 C++ 结构体。损坏槽会被忽略；NVS 不可用时仍可收发，保存失败会显示错误。

接收 DMA 缓冲为 512 个 RMT symbol，约 1024 段脉冲；缓冲恰好填满时按可能截断处理并丢弃。
20 ms 无边沿结束一帧，因此包含更长内部间隔的复杂遥控信号可能被拆成多帧。
原始信号每段允许 1–32767 us，总长最多 2 s。这里的“raw”是解调后的包络，
**无法从接收器输出自动测得原始载波频率**，未知遥控器应按实际载波设置后学习。
协议特定的多帧空调状态机、自动协议识别库等可在纯协议层扩展；并未声称支持全部遥控器。

应用通过 `infrared::Service` 提交 listen/learn/transmit_nec/replay/save/erase，
用 `copy_last_frame` 获取有界原始帧；自定义协议可构造 `protocol::Frame` 后提交
`transmit_raw`。原始发送会同步复制入服务的单个待发槽，返回后调用者可释放源 Frame；
已有原始发送排队时会返回忙碌，不会用裸指针引用调用者内存。

## 引脚、电源与资源规则

| 资源 | 连接 |
| --- | --- |
| ES8311 I2C | 0x18，共享内部 SDA47 / SCL48 |
| I2S MCLK / BCLK / LRCK | GPIO18 / GPIO17 / GPIO15 |
| ESP32 TX → codec DIN | GPIO14 |
| ESP32 RX ← codec DOUT | GPIO16 |
| 喇叭功放 | M5PM1 GPIO3 |
| IR TX / RX | GPIO46 / GPIO42 |
| IR 电源 | EXT_5V |

引脚方向以 ESP32 为基准。官方产品表中的 DIN/DOUT 行应结合原理图和官方驱动判断，
不能把 codec 端命名直接用作主控端 TX/RX。

BSP 对完整 PMIC 读改写操作加互斥锁。GPIO_FUNC0 每个引脚占两位，GPIO2 为 5:4、
GPIO3 为 7:6；LCD、充电输入和其他功能不会被功放操作覆盖。
原波形模块不再在启动时开启无关的 5V 供电，G4/G5 保持 3.3V 逻辑输出。

IR RX 和功放使用互斥租用，冲突返回 ESP_ERR_INVALID_STATE，不会强行中断另一个服务。
红外接收获准前，BSP 确保功放关闭并等待关断稳定。麦克风采集可与 IR RX 并存；
带喇叭输出的音频与 IR RX 不能并存。

红外收发按需租用 EXT_5V。若已由外部供电或升压原本已打开，退出时保持其状态；
若由本功能开启升压，最后一个租用者退出时关闭升压。开启前读取 PMIC ADC，
避免向已供电的 Grove/HAT EXT_5V 反向输出；模糊的低电压读数会拒绝开启。
供电切换不能替代正确接线：输出模式下不能同时从 Grove/HAT EXT_5V 输入电源。
检测只用于开启前判断，不能保证检测到输出期间新插入的外部电源。

## 构建和自动验证

在容器工程目录 `/workspace/esp32_idf/m5_sticks3` 执行：

```bash
source /opt/esp/idf/export.sh
bash tools/verify_peripherals.sh
```

脚本在 build 内使用独立 SDKCONFIG 和构建目录，不烧录硬件。等价主要步骤：

```bash
cmake -S tests -B build/peripheral-host-tests -G Ninja
cmake --build build/peripheral-host-tests
ctest --test-dir build/peripheral-host-tests --output-on-failure
cmake -S tests -B build/peripheral-sanitizers -G Ninja -DM5_HOST_SANITIZERS=ON
cmake --build build/peripheral-sanitizers
ctest --test-dir build/peripheral-sanitizers --output-on-failure
cp sdkconfig build/peripherals.sdkconfig
idf.py -B build/peripherals-idf -D SDKCONFIG="$PWD/build/peripherals.sdkconfig" build
```

主机测试覆盖原有生命周期/按键/游戏/通信，新 DSP 数值、16/24 位极值和分块连续性，
NEC 标准/扩展/重复帧、损坏 CRC、全部截断长度、最大原始帧和有界模糊输入，
以及真实 BSP 实现的寄存器保留、功放互斥、外部供电、失败 ACK、清理重试和并发调用。
主机桩仅模拟硬件返回，不替代实机 DMA/电气验证。

## 本轮验证记录（2026-09-09）

- 容器 `esp-dev`，ESP-IDF `v5.5.4`：完整固件构建通过，输出 `build/peripherals-idf/m5_sticks3.bin`。
- 六组主机测试通过；同六组 AddressSanitizer / UndefinedBehaviorSanitizer 检查通过。
- 真实 LVGL 离屏测试通过，覆盖 36 个页面状态、文字边界、首次录音、参数编辑、槽位管理、停止与退出。
- `bash tools/verify_peripherals.sh` 已端到端执行通过，日志在 `build/peripherals-verification.log`。
- 本轮未烧录，未执行任何实机音频、红外或电源切换验收。

## 接入设备后的验收

| 场景 | 应观察的结果 |
| --- | --- |
| SPEAKER 默认音调，改变音量和频率 | 无死机，音量变化有效，停止立即进入关断流程 |
| MICROPHONE 电平、5 s 录音、回到 SPEAKER 播放 | 电平随声音变化，录音时长合理，切页后采集停止，录音仍可播放 |
| 16/24 bit 及各采样率 | 时钟、音调频率与录放时长正确，无持续溢出；格式改变清除旧录音 |
| 高采样率长录音和内存不足 | 成功时播放完整，空间不足时明确报错且菜单仍可操作 |
| IR RX 配合已知 NEC 遥控器，距离至少 30 cm | 地址/命令正确，长按可识别 repeat，功放保持关闭 |
| IR 学习、重启、槽回放与删除 | 信号跨重启保存，回放有效，删除后不再显示已保存 |
| IR 活动中退出页面，随后播放音频 | RMT 停止并释放供电/功放租用，喇叭可再次启动 |
| 播放时切断 USB、电池供电 | 实际音量上限更新；检查是否存在重启、供电跌落或噪声 |
| Wi-Fi/BLE 活跃时录放及反复切页 | 观察 DMA 诊断、堆剩余量、界面响应和栈余量 |

## 依据

- [StickS3 官方产品资料与供电/IR 注意事项](https://docs.m5stack.com/zh_CN/core/StickS3)
- [M5PM1 芯片手册](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_CN.pdf)
- [StickS3 官方 RMT NEC 示例](https://docs.m5stack.com/en/arduino/m5sticks3/ir_nec)
- [ESP-IDF ES8311 官方示例](https://github.com/espressif/esp-idf/tree/v5.5.1/examples/peripherals/i2s/i2s_codec/i2s_es8311)
- [ESP32-S3 RMT 驱动文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/rmt.html)
