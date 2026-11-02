# BMI270 / VQF 跟手与回零

## 现象与原因

Roll/Pitch 转动后回到原姿态，仍剩约 1°，再缓慢回正，需要区分两种延迟：

- 原采样任务每 10 ms 读取 ACC、GYR 数据寄存器，不检查数据就绪，也不缓存错过的帧。
  BMI270 时钟与 ESP32 的任务时钟并不同步，重复/漏掉样本会造成陀螺仪积分误差。
  2026-11-02 的基线诊断记录中，45 秒内 4500 次更新有 274 次读取前 GYR DRDY 为零
  （6.09%）。这证明原采样方式存在读到旧数据的窗口，但不能仅凭静态数据把用户
  每次转动的全部残差都归因于此。
- 原 `tauAcc=2.0` 使倾角误差通过加速度参考缓慢修正；显示层又以 `gain=0.22`
  平滑融合角，在 16 ms 更新间隔下引入约 57 ms 的低频延迟、约 0.2 秒的 95% 阶跃收敛。
  显示平滑与数秒的融合校正拖尾是不同的现象。

VQF 的加速度滤波位于惯性参考系，`tauAcc` 不是对全部输出角度直接低通。
陀螺仪负责快速旋转跟踪，加速度负责长期倾角修正。参数定义见
[VQF 官方说明](https://vqf.readthedocs.io/en/stable/ref_cpp_params.html)。

## 当前实现

- BMI270 ACC/GYR 均设为 200 Hz，保留 ±4 g / ±1000 °/s 量程及性能模式。
- 启用硬件 FIFO 的无头 ACC+GYR 帧，选择经过传感器滤波、没有降采样的数据。
  每帧 12 字节，顺序为 GYR XYZ、ACC XYZ。
- CPU1 每 2 ms 轮询 FIFO，逐帧以 5 ms 步长调用 VQF；空 FIFO 不积分，任务迟到则
  依次处理积压的真实帧。UI 只读取最新结果。
- 读取并应用有符号的出厂 `GYR_CAS.factor_zx` 补偿，避免 Z 轴角速度串入 X 轴。
  数据量程换算用 32768 LSB。寄存器和补偿公式来自
  [Bosch BMI270 数据手册](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf)
  的 4.6.10、4.7、5.2 节。
- `tauAcc` 改为 0.5 秒，保留 VQF 运动/静止零偏估计。MOTION 直接显示融合结果，
  不再叠加 `0.22` 输出平滑，也没有角度死区、强制归零或把小角度当成静止。
- 检查传感器配置，等待 100 ms 启动稳定后清空 FIFO。FIFO 帧不完整、过量积压
  （超过 32 帧 / 标称 160 ms）或读取中途失败时，使发布结果失效并在下次读取前清空
  FIFO；清空失败会重试，不把残缺数据送入 VQF。100 ms 无有效样本也使快照失效。
  丢失期间的真实转动无法重建，恢复后仍须靠后续数据修正。

核心实现位于 `components/app_modules/include/motion_processing.hpp`、
`motion_fifo.hpp`、`src/motion_module.cpp` 和 `src/ui_module.cpp`。
没有修改 `managed_components` 或相邻工程的 VQF 源码。

缩短 `tauAcc` 会提高对加速度的信任；持续平移、猛烈甩动和振动时可能产生更明显的
暂态倾角误差。这是当前手持交互取向的权衡，可在 `fusion_parameters()` 中调整。
绝对水平还受加速度计零偏、安装角和机械放置影响；本次没有做六面加速度校准或
转台比例/时钟校准，不能保证物理绝对角度达到 0.01°。六轴 VQF 的 Yaw 也没有绝对航向参考。

## 验证记录（2026-11-02）

- ESP-IDF v5.5.4，Docker 容器 `esp-dev`，ESP32-S3 USB Serial/JTAG `/dev/ttyACM0`。
- 19 项主机测试通过。新增测试覆盖 FIFO 空读、重复数值的不同真实帧、批量读出、
  有符号单位换算、串轴补偿、配置不一致、残缺帧、过量积压、I2C 部分读取及清空失败恢复。
  不规则批次的实际 FIFO 解析/融合也验证了 60° 已知旋转。
- 使用相同 VQF 单精度实现，模拟静置 8 秒、0.3 秒转 30°、保持 1 秒、0.3 秒回位，
  加入 2% 陀螺仪比例误差和 0.3 °/s 零偏。回位 1 秒后，Roll/Pitch 残差均由
  `tauAcc=2.0` 的 0.2421° 降至 `tauAcc=0.5` 的 0.0092°。这是仿真回归，非实机角度精度。
- 修复版静态实机记录持续 100.041 秒，处理 19824 帧，即约 198.158 Hz；FIFO 重置
  为零，最大批次 1 帧。该记录 Roll/Pitch 峰峰波动分别为 0.0559° / 0.0495°。
  实测频率与标称 200 Hz 有差异，本次未校准传感器时钟及比例误差。
- 三款小游戏实机冒烟通过：TILT QUEST / METEOR DODGE 持续收到有效姿态；
  切换 TAP RUNNER 后 BMI270 停止并释放，遥测变为无效，随后回到主菜单。
  该设备读取的出厂串轴系数为 `-7/512`。
- 正式固件已构建并烧录，全部诊断/smoke 开关为 OFF，启动后停留主菜单，IMU 按需开启。
  本次二进制为 `build/motion-idf/m5_sticks3.bin`，测量日志保存在 `build/motion-results/`。
- 尚无转台或已知角度实机往返轨迹，手持 30° 回位残差需实操确认；静态数据不能代替动态验收。

主机复现（容器工程目录，先加载 `/opt/esp-idf/export.sh`）：

```bash
cmake -S tests -B build/motion-host-tests -G Ninja
cmake --build build/motion-host-tests
ctest --test-dir build/motion-host-tests --output-on-failure
build/motion-host-tests/motion_tests
```

## 独立串口诊断

诊断开关默认关闭，与其它实机 smoke 开关互斥。启用后自动启动 IMU，每约 50 ms
输出一行 `motion_diag`，不会改变传感器 200 Hz 的融合周期。
离开 MOTION 或依赖 IMU 的游戏仍会按正常生命周期关闭传感器，可重新进入 MOTION 启动。

以下在容器工程目录执行，避免改写正式 `sdkconfig`：

```bash
source /opt/esp-idf/export.sh
mkdir -p build
cp sdkconfig build/motion-diagnostics.sdkconfig
idf.py -B build/motion-diagnostics \
  -D SDKCONFIG="$PWD/build/motion-diagnostics.sdkconfig" \
  -D M5_STICKS3_MOTION_DIAGNOSTICS=ON \
  -D M5_STICKS3_HW_SMOKE_TEST=OFF \
  -D M5_STICKS3_RADIO_SMOKE_TEST=OFF \
  -D M5_STICKS3_DAP_SMOKE_TEST=OFF build
idf.py -B build/motion-diagnostics -p /dev/ttyACM0 flash monitor
```

CSV 字段：

```text
us,samples,ax,ay,az,gx,gy,gz,roll,pitch,yaw,bx,by,bz,rest,fifo_resets,max_batch
```

`us` 是主机 ESP32 时间戳，`samples` 是真实融合帧累计数；加速度单位 g，角速度和
零偏单位 °/s，角度单位 °。一行可以跨过多个融合样本，串口行数不是采样率。

动态验收时先静置至 `rest=1`，分别做 Roll/Pitch 30° 往返，每次回原位置保持 5 秒。
用机械止挡或稳定参考面比较回位前后姿态，并同时查看 `samples` 与 `fifo_resets`。
完成后烧回关闭诊断和全部 smoke 开关的正常固件，防止自动启动 IMU 和连续串口输出。
