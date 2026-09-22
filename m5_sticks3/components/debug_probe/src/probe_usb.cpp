#include "probe_usb.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

namespace {
using debug_probe::Packet;
StaticQueue_t queue_control;
uint8_t queue_storage[2 * sizeof(Packet)];
QueueHandle_t incoming = nullptr;
std::atomic<bool> attached{false}, disconnected{false}, accepting{false};
bool installed = false;
usb_phy_handle_t serial_phy = nullptr;
char serial[13]{};
const char* strings[] = {"\x09\x04", "M5StickS3", "StickS3 CMSIS-DAP", serial, "CMSIS-DAP v2"};
const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x4004,
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    // CMSIS-DAP requires OUT then IN descriptors, vendor class 0xff/0/0.
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
};
const uint8_t ms_os_20[] = {
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xb2, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xa8, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x14, 0x00, 0x03, 0x00, 0x57, 0x49,
    0x4e, 0x55, 0x53, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0x00,
    0x04, 0x00, 0x07, 0x00, 0x2a, 0x00, 0x44, 0x00, 0x65, 0x00, 0x76, 0x00, 0x69, 0x00, 0x63, 0x00,
    0x65, 0x00, 0x49, 0x00, 0x6e, 0x00, 0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00,
    0x63, 0x00, 0x65, 0x00, 0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x7b, 0x00, 0x43, 0x00, 0x44, 0x00, 0x42, 0x00, 0x33, 0x00, 0x42, 0x00, 0x35, 0x00,
    0x41, 0x00, 0x44, 0x00, 0x2d, 0x00, 0x32, 0x00, 0x39, 0x00, 0x33, 0x00, 0x42, 0x00, 0x2d, 0x00,
    0x34, 0x00, 0x36, 0x00, 0x36, 0x00, 0x33, 0x00, 0x2d, 0x00, 0x41, 0x00, 0x41, 0x00, 0x33, 0x00,
    0x36, 0x00, 0x2d, 0x00, 0x31, 0x00, 0x41, 0x00, 0x41, 0x00, 0x45, 0x00, 0x34, 0x00, 0x36, 0x00,
    0x34, 0x00, 0x36, 0x00, 0x33, 0x00, 0x37, 0x00, 0x37, 0x00, 0x36, 0x00, 0x7d, 0x00, 0x00, 0x00,
    0x00, 0x00
};
const uint8_t bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(sizeof(ms_os_20), 0x20),
};
void event(tinyusb_event_t* e, void*) {
    if (e->id == TINYUSB_EVENT_ATTACHED) attached.store(true);
    if (e->id == TINYUSB_EVENT_DETACHED || e->id == TINYUSB_EVENT_SUSPENDED) {
        attached.store(false);
        disconnected.store(true);
        if (incoming) xQueueReset(incoming);
    }
    if (e->id == TINYUSB_EVENT_RESUMED) attached.store(true);
}
esp_err_t restore_serial() {
    if (serial_phy) return ESP_OK;
    usb_phy_config_t cfg{};
    cfg.controller = USB_PHY_CTRL_SERIAL_JTAG;
    cfg.target = USB_PHY_TARGET_INT;
    return usb_new_phy(&cfg, &serial_phy);
}
} // namespace
extern "C" const uint8_t* tud_descriptor_bos_cb(void) { return bos; }
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t* req) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (req->bmRequestType == 0xc0 && req->bRequest == 0x20 && req->wIndex == 7)
        return tud_control_xfer(rhport, req, const_cast<uint8_t*>(ms_os_20), sizeof(ms_os_20));
    return false;
}
extern "C" void tud_vendor_rx_cb(uint8_t, const uint8_t* buffer, uint16_t size) {
    if (!accepting.load() || !incoming || !size || size > debug_probe::kPacketSize) return;
    if (buffer[0] == 7) { debug_probe::request_transfer_abort(debug_probe::Mode::usb); return; }
    Packet packet{};
    packet.size = size;
    std::memcpy(packet.data, buffer, size);
    // Packet count is advertised as one. Protocol-violating overflow terminates
    // the session rather than silently executing a request with a lost reply.
    if (xQueueSend(incoming, &packet, 0) != pdTRUE) {
        xQueueReset(incoming);
        disconnected.store(true);
    }
}
namespace debug_probe {
esp_err_t usb_start() {
    if (installed) return ESP_ERR_INVALID_STATE;
    if (!incoming) incoming = xQueueCreateStatic(2, sizeof(Packet), queue_storage, &queue_control);
    xQueueReset(incoming);
    disconnected.store(false); attached.store(false);
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if (serial_phy) {
        const auto err = usb_del_phy(serial_phy);
        if (err != ESP_OK) return err;
        serial_phy = nullptr;
    }
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &device;
    cfg.descriptor.full_speed_config = configuration;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof(strings)/sizeof(strings[0]);
    cfg.event_cb = event;
    accepting.store(true);
    const auto err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) { accepting.store(false); restore_serial(); return err; }
    installed = true;
    return ESP_OK;
}
esp_err_t usb_stop() {
    accepting.store(false);
    if (installed) {
        tud_disconnect();
        const auto err = tinyusb_driver_uninstall();
        if (err != ESP_OK) return err;
        installed = false;
    }
    attached.store(false);
    if (incoming) xQueueReset(incoming);
    return restore_serial();
}
bool usb_receive(Packet& p) { return incoming && xQueueReceive(incoming, &p, 0) == pdTRUE; }
bool usb_send(const Packet& p) { return usb_connected() && tud_vendor_write(p.data, p.size) == p.size; }
bool usb_connected() { return attached.load() && !disconnected.load(); }
// The detach callback already cleared the queue. Do not erase the first
// request of a newly enumerated host that arrived before this worker tick.
bool usb_take_disconnect() { return disconnected.exchange(false); }

} // namespace debug_probe
