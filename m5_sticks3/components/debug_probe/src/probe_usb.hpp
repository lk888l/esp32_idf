#pragma once
#include "debug_probe.hpp"
namespace debug_probe {
esp_err_t usb_start();
esp_err_t usb_stop();
bool usb_receive(Packet& packet);
bool usb_send(const Packet& packet);
bool usb_connected();
bool usb_take_disconnect();
}
