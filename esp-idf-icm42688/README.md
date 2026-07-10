# esp-idf-espc5 ICM42688 demo

这是一个基于 `esp_idf_template` 拆出来的 ESP32-C5 ESP-IDF C++ 示例工程，用 I2C 驱动 ICM42688 并周期打印陀螺仪、加速度计和温度数据。

## 目录结构

```text
.
├── main/                  # app_main，仅注册应用模块
└── components/
    ├── app/               # AppModule/AppManager/AppTask 框架
    ├── app_modules/       # 陀螺仪读取打印演示模块
    ├── bsp/               # 板级 I2C 总线和引脚配置
    ├── hardware/          # ICM42688 传感器驱动
    ├── third_party/vqf/   # VQF 姿态解算算法，MIT license
    ├── base/
    └── logger/
```

## 分层

- `components/bsp`: 定义平台无关 I2C 抽象接口，并提供 ESP-IDF I2C adapter。默认 I2C0、SDA GPIO8、SCL GPIO9、400 kHz、ICM42688 地址 `0x68`。如硬件连接不同，修改 `components/bsp/include/bsp_board.hpp`。
- `components/hardware/icm42688`: 平台无关 ICM42688 驱动，只依赖标准 C++ 和 `bsp::I2CDevice` 抽象接口，负责寄存器读写、WHO_AM_I 检查、复位、量程配置和采样换算。
- `components/third_party/vqf`: GitHub 开源 VQF C++ 算法源码，作为独立第三方 component 引入。
- `components/app_modules`: `GyroReaderModule` 负责实例化 ESP-IDF BSP I2C、创建 I2C device、注入 FreeRTOS delay，并启动 task 周期读取，调用 VQF 进行 6D 姿态解算后打印。
- `main`: 只注册 `createGyroReaderModule()` 并运行 `AppManager` 主循环。

## 构建

目标芯片默认是 `esp32c5`，配置在 `sdkconfig.defaults`。

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```
