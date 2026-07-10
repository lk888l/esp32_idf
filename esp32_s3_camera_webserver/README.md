# ESP32-S3 Camera Web Server

ESP-IDF implementation of an Arduino-style CameraWebServer example for the
Freenove ESP32-S3 WROOM camera board.

The project keeps the local `esp_idf_template` shape:

- `main` registers application modules.
- `components/bsp` owns board pin definitions.
- `components/camera` wraps `esp32-camera`.
- `components/wifi` connects the board as a Wi-Fi station.
- `components/camera_web_server` exposes the browser endpoints.

## Board Pins

The camera pins are from the provided Freenove pinout image:

| Signal | GPIO |
| --- | ---: |
| CAM_SIOD | 4 |
| CAM_SIOC | 5 |
| CAM_VSYNC | 6 |
| CAM_HREF | 7 |
| CAM_XCLK | 15 |
| CAM_Y9 | 16 |
| CAM_Y8 | 17 |
| CAM_Y7 | 18 |
| CAM_Y6 | 12 |
| CAM_Y5 | 10 |
| CAM_Y4 | 8 |
| CAM_Y3 | 9 |
| CAM_Y2 | 11 |
| CAM_PCLK | 13 |

The board pinout does not show camera PWDN or RESET pins, so both are set to
`GPIO_NUM_NC`.

## Configure

Set the ESP-IDF environment first. From Git Bash/MSYS:

```bash
source /c/kk_software/toolchain/esp_idf/frameworks/esp-idf-v5.5.4/export.sh
```

Then configure Wi-Fi credentials:

```bash
cd /c/kk_data/code/esp32/esp32_idf/esp32_s3_camera_webserver
idf.py set-target esp32s3
idf.py menuconfig
```

Edit:

```text
Camera Web Server -> Wi-Fi SSID
Camera Web Server -> Wi-Fi password
```

Defaults are in `sdkconfig.defaults`, but they are placeholder values.

On this Windows setup, the helper scripts in this project are the simplest path:

```bat
menuconfig.bat
build.bat
flash_monitor.bat COMx
```

## Build and Flash

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

After the board connects to Wi-Fi, the monitor prints the assigned IP address.
Open that IP address in a browser.

## HTTP Endpoints

- `/` browser page with live stream
- `/stream` MJPEG stream
- `/capture` single JPEG frame
- `/status` JSON status

The default camera frame size is `QSXGA 2560x1920`, the highest frame size
advertised by `esp32-camera` for OV5640. If the hotspot link is too slow, lower
`Camera Web Server -> Camera frame size` in `menuconfig.bat`.
