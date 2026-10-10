#include "sleep_ui.hpp"

#include <cstdio>
#include <cstring>

#include "clock_service.hpp"
#include "sleep_service.hpp"

namespace app_modules {
namespace {
constexpr uint32_t kCountdownMs = 3000;
constexpr uint32_t kTimerSeconds[] = {0, 300, 1800, 3600};
constexpr const char* kModeNames[] = {"BUTTONS", "5 MIN", "30 MIN", "1 HOUR"};

lv_obj_t* label(lv_obj_t* parent, const char* value, const lv_font_t* font,
                int y, uint32_t color)
{
    auto* object = lv_label_create(parent);
    lv_obj_set_pos(object, 0, y);
    lv_obj_set_width(object, lv_pct(100));
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, value);
    return object;
}
void text(lv_obj_t* object, const char* value)
{
    if (std::strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}
} // namespace

lv_obj_t* SleepUi::open()
{
    if (screen_) return screen_;
    screen_ = lv_obj_create(nullptr);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(screen_, 135, 240);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x0D1025), 0);
    lv_obj_set_style_bg_grad_color(screen_, lv_color_hex(0x231C3C), 0);
    lv_obj_set_style_bg_grad_dir(screen_, LV_GRAD_DIR_VER, 0);
    auto* title = label(screen_, "SLEEP", &lv_font_montserrat_14, 8, 0xF3EDFF);
    lv_obj_set_pos(title, 8, 8); lv_obj_set_width(title, 68);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    auto* level = label(screen_, "L2", &lv_font_montserrat_12, 9, 0xC4B5FD);
    lv_obj_set_pos(level, 90, 9); lv_obj_set_width(level, 37);
    lv_obj_set_style_text_align(level, LV_TEXT_ALIGN_RIGHT, 0);
    label(screen_, "DEEP SLEEP", &lv_font_montserrat_12, 35, 0xB4A9CF);

    auto* panel = lv_obj_create(screen_);
    lv_obj_set_pos(panel, 8, 59); lv_obj_set_size(panel, 119, 94);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x2E2548), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x695887), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    heading_ = label(panel, "WAKE MODE", &lv_font_montserrat_12, 11, 0xB4A9CF);
    mode_ = label(panel, "BUTTONS", &lv_font_montserrat_16, 37, 0xF3EDFF);
    wake_ = label(panel, "K1 / K2 wake", &lv_font_montserrat_12, 70, 0xC4B5FD);
    status_ = label(screen_, "RTC KEEPS TIME", &lv_font_montserrat_12, 165, 0xC4B5FD);
    time_ = label(screen_, "--:--:--", &lv_font_montserrat_16, 186, 0xF3EDFF);
    controls_ = label(screen_, "K1 BACK  K2 MODE", &lv_font_montserrat_12, 207, 0xB4A9CF);
    hint_ = label(screen_, "HOLD K2 TO SLEEP", &lv_font_montserrat_12, 224, 0xB4A9CF);
    selection_ = 0;
    countdown_ = submitted_ = refused_ = false;
    update();
    return screen_;
}

void SleepUi::close()
{
    countdown_ = submitted_ = refused_ = false;
    if (screen_) lv_obj_delete(screen_);
    screen_ = heading_ = mode_ = wake_ = status_ = time_ = controls_ = hint_ = nullptr;
}

void SleepUi::arm(uint32_t timer_seconds)
{
    if (!screen_ || countdown_ || submitted_) return;
    timer_seconds_ = timer_seconds;
    countdown_start_ = lv_tick_get();
    countdown_ = true;
    refused_ = false;
    update();
}

bool SleepUi::select(bool long_press)
{
    if (!screen_ || countdown_ || submitted_) return false;
    if (long_press) {
        arm(kTimerSeconds[selection_]);
    } else {
        selection_ = (selection_ + 1) % 4;
        refused_ = false;
        update();
    }
    return false;
}

bool SleepUi::back()
{
    if (countdown_) {
        countdown_ = false;
        update();
        return false;
    }
    return !submitted_;
}

#ifdef M5_STICKS3_DAP_SMOKE_TEST
void SleepUi::start_for_test(uint32_t timer_seconds)
{
    arm(timer_seconds);
}
#endif

void SleepUi::update()
{
    if (!screen_) return;
    const uint32_t elapsed = lv_tick_get() - countdown_start_;
    if (countdown_ && elapsed >= kCountdownMs) {
        // Hand off one request to the application task; no hardware work under LVGL.
        countdown_ = false;
        submitted_ = sleep_service::request_sleep(timer_seconds_);
        refused_ = !submitted_;
    }
    const auto error = sleep_service::last_error();
    if (submitted_ && error != ESP_OK) {
        submitted_ = false;
        refused_ = true;
    }
    if (countdown_) {
        char remaining[12]{};
        std::snprintf(remaining, sizeof(remaining), "%lu", static_cast<unsigned long>(
            (kCountdownMs - elapsed + 999) / 1000));
        text(heading_, "SLEEP IN");
        lv_obj_set_y(mode_, 27);
        lv_obj_set_style_text_font(mode_, &lv_font_montserrat_36, 0);
        text(mode_, remaining);
        text(wake_, "seconds to sleep");
        text(controls_, "K1 CANCEL");
        text(hint_, "Release buttons");
    } else if (submitted_) {
        text(heading_, "DEEP SLEEP");
        lv_obj_set_y(mode_, 33);
        lv_obj_set_style_text_font(mode_, &lv_font_montserrat_24, 0);
        text(mode_, "SLEEP");
        text(wake_, "Starting...");
        text(controls_, "RTC keeps running");
        text(hint_, "K1 / K2 to wake");
    } else {
        text(heading_, "WAKE MODE");
        lv_obj_set_y(mode_, selection_ ? 33 : 37);
        lv_obj_set_style_text_font(mode_, selection_ ? &lv_font_montserrat_24 : &lv_font_montserrat_16, 0);
        text(mode_, kModeNames[selection_]);
        text(wake_, selection_ ? "Timer + buttons" : "K1 / K2 wake");
        text(controls_, "K1 BACK  K2 MODE");
        text(hint_, "HOLD K2 TO SLEEP");
    }
    const auto clock = clock_service::snapshot();
    text(time_, clock.time);
    text(status_, refused_ ? (error == ESP_ERR_INVALID_STATE ? "RELEASE KEYS" :
        error == ESP_OK ? "SLEEP BUSY" : "SLEEP FAILED") :
        clock.valid ? "RTC KEEPS TIME" : "RTC NOT SYNCED");
}
} // namespace app_modules
