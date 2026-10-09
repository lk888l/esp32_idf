#include "clock_ui.hpp"

#include <cstdio>
#include <cstring>

namespace app_modules {
namespace {
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                int y, uint32_t color)
{
    auto* object = lv_label_create(parent);
    lv_obj_set_pos(object, 0, y);
    lv_obj_set_width(object, lv_pct(100));
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, text);
    return object;
}
void text(lv_obj_t* object, const char* value)
{
    if (std::strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}
} // namespace

lv_obj_t* ClockUi::open()
{
    if (screen_) return screen_;
    screen_ = lv_obj_create(nullptr);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(screen_, 135, 240);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x08121F), 0);
    lv_obj_set_style_bg_grad_color(screen_, lv_color_hex(0x152C3D), 0);
    lv_obj_set_style_bg_grad_dir(screen_, LV_GRAD_DIR_VER, 0);
    auto* title = label(screen_, "CLOCK", &lv_font_montserrat_14, 8, 0xE8F5FF);
    lv_obj_set_pos(title, 8, 8); lv_obj_set_width(title, 57);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    auto* zone = label(screen_, clock_service::timezone_label(), &lv_font_montserrat_12, 9, 0x67E8F9);
    lv_obj_set_pos(zone, 65, 9); lv_obj_set_width(zone, 62);
    lv_obj_set_style_text_align(zone, LV_TEXT_ALIGN_RIGHT, 0);
    label(screen_, "NETWORK TIME", &lv_font_montserrat_12, 35, 0x91A7BC);

    auto* panel = lv_obj_create(screen_);
    lv_obj_set_pos(panel, 8, 59); lv_obj_set_size(panel, 119, 94);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x173347), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x365B70), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    hours_ = label(panel, "--:--", &lv_font_montserrat_36, 9, 0xF2FAFF);
    seconds_ = label(panel, "--", &lv_font_montserrat_16, 55, 0x67E8F9);
    progress_ = lv_bar_create(panel);
    lv_obj_set_pos(progress_, 12, 82); lv_obj_set_size(progress_, 93, 3);
    lv_bar_set_range(progress_, 0, 59);
    lv_obj_set_style_bg_color(progress_, lv_color_hex(0x294959), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_, lv_color_hex(0x67E8F9), LV_PART_INDICATOR);
    date_ = label(screen_, "----/--/-- ---", &lv_font_montserrat_12, 166, 0xD2E2EF);
    status_ = label(screen_, "WAIT FOR WIFI", &lv_font_montserrat_12, 190, 0xFBBF24);
    age_ = label(screen_, "Connect in WIFI", &lv_font_montserrat_12, 207, 0x91A7BC);
    label(screen_, "K1 BACK  K2 SYNC", &lv_font_montserrat_12, 224, 0x718BA2);
    message_until_ = 0;
    refresh_ = lv_tick_get() - 1000;
    update();
    return screen_;
}

void ClockUi::close()
{
    if (screen_) lv_obj_delete(screen_);
    screen_ = hours_ = seconds_ = progress_ = date_ = status_ = age_ = nullptr;
}

bool ClockUi::select(bool long_press)
{
    if (long_press) return true;
    request_error_ = clock_service::request_sync();
    message_until_ = lv_tick_get() + 3000;
    refresh_ = lv_tick_get() - 1000;
    update();
    return false;
}

void ClockUi::update()
{
    if (!screen_) return;
    const uint32_t now = lv_tick_get();
    if (now - refresh_ < 1000) return;
    refresh_ = now;
    const auto value = clock_service::snapshot();
    char hours[6]{};
    std::memcpy(hours, value.time, 5);
    text(hours_, hours);
    text(seconds_, value.time + 6);
    lv_bar_set_value(progress_, value.valid ? (value.time[6] - '0') * 10 + value.time[7] - '0' : 0, LV_ANIM_OFF);
    char date[32]{};
    std::snprintf(date, sizeof(date), "%s %s", value.date, value.weekday);
    text(date_, date);
    const char* status = !value.online ? (value.valid ? "WIFI OFFLINE" : "WAIT FOR WIFI") :
        value.state == clock_service::State::syncing ? "SYNCING..." :
        value.state == clock_service::State::retrying ? "NTP RETRYING" :
        value.valid ? "NTP SYNCED" : "WAIT FOR NTP";
    text(status_, status);
    lv_obj_set_style_text_color(status_, lv_color_hex(
        value.online && value.state == clock_service::State::synced ? 0x34D399 : 0xFBBF24), 0);
    char detail[48]{};
    if (message_until_ && static_cast<int32_t>(message_until_ - now) > 0) {
        std::snprintf(detail, sizeof(detail), "%s", request_error_ == ESP_OK ? "Sync requested" :
            request_error_ == ESP_ERR_INVALID_STATE ? "Connect in WIFI" : "Try again shortly");
    } else if (!value.online) {
        std::snprintf(detail, sizeof(detail), "%s", value.valid ? "Time keeps running" : "Connect in WIFI");
    } else if (value.sync_age_s < 0) {
        std::snprintf(detail, sizeof(detail), "%s", value.state == clock_service::State::retrying ? "Check internet" : "Waiting for server");
    } else if (value.sync_age_s < 60) {
        std::snprintf(detail, sizeof(detail), "Updated %llds ago", static_cast<long long>(value.sync_age_s));
    } else if (value.sync_age_s < 3600) {
        std::snprintf(detail, sizeof(detail), "Updated %lldm ago", static_cast<long long>(value.sync_age_s / 60));
    } else {
        std::snprintf(detail, sizeof(detail), "Updated %lldh ago", static_cast<long long>(value.sync_age_s / 3600));
    }
    text(age_, detail);
}
} // namespace app_modules
