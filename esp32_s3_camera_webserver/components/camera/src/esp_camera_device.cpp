#include "esp_camera_device.hpp"

#include "bsp_board.hpp"
#include "sdkconfig.h"

namespace {

framesize_t configured_frame_size()
{
#if CONFIG_CAMERA_WEB_FRAMESIZE_QVGA
    return FRAMESIZE_QVGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_SVGA
    return FRAMESIZE_SVGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_XGA
    return FRAMESIZE_XGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_SXGA
    return FRAMESIZE_SXGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_UXGA
    return FRAMESIZE_UXGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_QXGA
    return FRAMESIZE_QXGA;
#elif CONFIG_CAMERA_WEB_FRAMESIZE_QSXGA
    return FRAMESIZE_QSXGA;
#else
    return FRAMESIZE_VGA;
#endif
}

} // namespace

namespace camera {

bool EspCameraDevice::initialize()
{
    if (initialized_) {
        return true;
    }

    const auto& pins = bsp::kCameraPins;

    camera_config_t config{};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = pins.y2;
    config.pin_d1 = pins.y3;
    config.pin_d2 = pins.y4;
    config.pin_d3 = pins.y5;
    config.pin_d4 = pins.y6;
    config.pin_d5 = pins.y7;
    config.pin_d6 = pins.y8;
    config.pin_d7 = pins.y9;
    config.pin_xclk = pins.xclk;
    config.pin_pclk = pins.pclk;
    config.pin_vsync = pins.vsync;
    config.pin_href = pins.href;
    config.pin_sccb_sda = pins.siod;
    config.pin_sccb_scl = pins.sioc;
    config.pin_pwdn = pins.pwdn;
    config.pin_reset = pins.reset;
    config.xclk_freq_hz = 20000000;
    config.frame_size = configured_frame_size();
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = CONFIG_CAMERA_WEB_JPEG_QUALITY;
    config.fb_count = CONFIG_CAMERA_WEB_FB_COUNT;

    const esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        log_.error("esp_camera_init failed: 0x{:x}", static_cast<int>(ret));
        return false;
    }

    sensor_t* cam_sensor = esp_camera_sensor_get();
    if (cam_sensor != nullptr) {
        int vflip = 0;
        int hmirror = 0;
#if CONFIG_CAMERA_WEB_VFLIP
        vflip = 1;
#endif
#if CONFIG_CAMERA_WEB_HMIRROR
        hmirror = 1;
#endif
        cam_sensor->set_vflip(cam_sensor, vflip);
        cam_sensor->set_hmirror(cam_sensor, hmirror);
    }

    initialized_ = true;
    log_.info("initialized, frame_size={}, jpeg_quality={}, fb_count={}",
              static_cast<int>(config.frame_size),
              config.jpeg_quality,
              config.fb_count);
    return true;
}

bool EspCameraDevice::deinitialize()
{
    if (!initialized_) {
        return true;
    }

    const esp_err_t ret = esp_camera_deinit();
    if (ret != ESP_OK) {
        log_.error("esp_camera_deinit failed: 0x{:x}", static_cast<int>(ret));
        return false;
    }

    initialized_ = false;
    return true;
}

camera_fb_t* EspCameraDevice::capture()
{
    if (!initialized_) {
        return nullptr;
    }

    return esp_camera_fb_get();
}

void EspCameraDevice::release(camera_fb_t* frame)
{
    if (frame != nullptr) {
        esp_camera_fb_return(frame);
    }
}

sensor_t* EspCameraDevice::sensor() const
{
    if (!initialized_) {
        return nullptr;
    }

    return esp_camera_sensor_get();
}

} // namespace camera
