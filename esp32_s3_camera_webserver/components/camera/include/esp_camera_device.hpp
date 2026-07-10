#pragma once

#include "esp_camera.h"
#include "logger.hpp"

namespace camera {

class EspCameraDevice {
public:
    bool initialize();
    bool deinitialize();

    camera_fb_t* capture();
    void release(camera_fb_t* frame);

    bool is_initialized() const { return initialized_; }
    sensor_t* sensor() const;

private:
    ium::Logger log_{"camera"};
    bool initialized_ = false;
};

} // namespace camera
