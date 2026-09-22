#include "debug_ui.hpp"
#include "connectivity_runtime.hpp"
#include <cstdio>
#include <cstring>
namespace app_modules {
namespace {
lv_obj_t* label(lv_obj_t* screen, int y, int height, const lv_font_t* font, uint32_t color) {
    auto* obj = lv_label_create(screen);
    if (!obj) return nullptr;
    lv_obj_set_pos(obj, 7, y); lv_obj_set_size(obj, 121, height);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_text(obj, ""); return obj;
}
void text(lv_obj_t* label, const char* value) {
    if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}
}
lv_obj_t* DebugUi::open(debug_probe::Mode mode) {
    if (screen_) return screen_;
    screen_ = lv_obj_create(nullptr);
    if (!screen_) return nullptr;
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x07111f), 0);
    lv_obj_set_style_bg_grad_color(screen_, lv_color_hex(0x102d35), 0);
    lv_obj_set_style_bg_grad_dir(screen_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0); lv_obj_set_style_border_width(screen_, 0, 0);
    title_ = label(screen_, 6, 16, &lv_font_montserrat_12, 0x67e8f9);
    status_ = label(screen_, 28, 20, &lv_font_montserrat_14, 0x34d399);
    clock_ = label(screen_, 54, 30, &lv_font_montserrat_24, 0xe8eef8);
    hint_ = label(screen_, 87, 16, &lv_font_montserrat_12, 0x95a4bd);
    details_ = label(screen_, 110, 72, &lv_font_montserrat_12, 0xe8eef8);
    action_ = label(screen_, 187, 21, &lv_font_montserrat_12, 0xfbbf24);
    auto* footer = label(screen_, 210, 30, &lv_font_montserrat_12, 0x95a4bd);
    if (!title_ || !status_ || !clock_ || !hint_ || !details_ || !action_ || !footer) {
        lv_obj_delete(screen_); screen_ = nullptr; return nullptr;
    }
    lv_obj_set_style_text_line_space(details_, 3, 0);
    lv_label_set_long_mode(action_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    text(footer, "K1 NEXT  K2 OK\nHOLD K2 BACK");
    mode_ = mode; selected_ = 0; wiring_ = paused_ = false; control_error_ = ESP_OK;
    debug_probe::set_clock_limit(debug_probe::kClockPresets[preset_]);
    debug_probe::select_mode(mode_);
    refresh_ = 0; update(); return screen_;
}
void DebugUi::close() {
    debug_probe::select_mode(debug_probe::Mode::off);
    if (screen_) lv_obj_delete(screen_);
    screen_ = nullptr; mode_ = debug_probe::Mode::off;
}
void DebugUi::next() { selected_ = (selected_ + 1) % (mode_ == debug_probe::Mode::usb ? 4 : 5); refresh_ = 0; update(); }
bool DebugUi::select(bool long_press) {
    if (long_press || selected_ == 3) return true;
    if (selected_ == 0) { preset_ = (preset_ + 1) % debug_probe::kClockPresets.size(); debug_probe::set_clock_limit(debug_probe::kClockPresets[preset_]); }
    if (selected_ == 1) wiring_ = !wiring_;
    if (selected_ == 2) { paused_ = !paused_; debug_probe::select_mode(paused_ ? debug_probe::Mode::off : mode_); }
    if (selected_ == 4) {
        connectivity::ControlRequest request{};
        request.action = mode_ == debug_probe::Mode::wifi ? connectivity::ControlAction::wifi_enable : connectivity::ControlAction::ble_enable;
        control_error_ = connectivity::request_control(request);
    }
    refresh_ = 0; update(); return false;
}
void DebugUi::update() {
    if (!screen_ || (refresh_ && lv_tick_get() - refresh_ < 150)) return;
    refresh_ = lv_tick_get();
    const auto state = debug_probe::snapshot();
    const auto radio = connectivity::snapshot();
    const uint32_t ceiling = debug_probe::kClockPresets[preset_];
    text(title_, debug_probe::mode_name(mode_));
    const bool radio_off = (mode_ == debug_probe::Mode::wifi && !radio.wifi.ap_active && radio.wifi.state == connectivity::WifiState::stopped) ||
                           (mode_ == debug_probe::Mode::ble && !radio.ble.enabled);
    text(status_, paused_ ? "PAUSED" : control_error_ != ESP_OK ? "RADIO ERROR" : radio_off ? "RADIO OFF" :
        state.error != ESP_OK ? "START ERROR" : state.mode != mode_ || !state.ready ? "STARTING" :
        state.swd ? "SWD ACTIVE" : state.connected ? "HOST LINK" : "WAITING HOST");
    char buffer[200];
    snprintf(buffer, sizeof(buffer), "%lu kHz", static_cast<unsigned long>(ceiling / 1000)); text(clock_, buffer);
    text(hint_, "SWD speed limit");
    if (wiring_) {
        text(details_, "G6 CLK / G7 DIO\nG8 NRST / GND\n3.3V target only\nPower target itself");
    } else if (state.error != ESP_OK || control_error_ != ESP_OK) {
        snprintf(buffer, sizeof(buffer), "Error 0x%X\nPause / resume\nto retry", unsigned(state.error != ESP_OK ? state.error : control_error_)); text(details_, buffer);
    } else if (mode_ == debug_probe::Mode::wifi) {
        const char* ip = radio.wifi.state == connectivity::WifiState::connected ? radio.wifi.address : radio.wifi.ap_address;
        snprintf(buffer, sizeof(buffer), "%s\nTCP 4441 / %lu k\n%lu pkt / %lu err\nACK %02X / %lu us", ip[0] ? ip : "No WiFi address",
            static_cast<unsigned long>(state.clock_hz/1000), static_cast<unsigned long>(state.packets),
            static_cast<unsigned long>(state.errors), state.last_ack, static_cast<unsigned long>(state.last_us)); text(details_, buffer);
    } else if (mode_ == debug_probe::Mode::ble) {
        snprintf(buffer, sizeof(buffer), "MTU %u / %s\nSWD cfg %lu k\n%lu pkt / %lu err\nPC bridge needed", radio.ble.mtu, radio.ble.encrypted ? "ENC" : "PAIR",
            static_cast<unsigned long>(state.clock_hz/1000), static_cast<unsigned long>(state.packets), static_cast<unsigned long>(state.errors)); text(details_, buffer);
    } else {
        snprintf(buffer, sizeof(buffer), "Bulk FS / 64 bytes\nSWD cfg %lu k\n%lu pkt / %lu err\nACK %02X / %lu us", static_cast<unsigned long>(state.clock_hz/1000),
            static_cast<unsigned long>(state.packets), static_cast<unsigned long>(state.errors), state.last_ack, static_cast<unsigned long>(state.last_us)); text(details_, buffer);
    }
    const char* actions[] = {"> Change limit", "> Wiring / stats", paused_ ? "> Resume probe" : "> Pause probe", "> Back to menu", "> Enable radio"};
    text(action_, actions[selected_]);
}
}
