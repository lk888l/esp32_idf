#include "app_modules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <string_view>

#include "app_module.hpp"
#include "bsp_display.hpp"
#include "button_event_bus.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "mini_games.hpp"
#include "motion_runtime.hpp"
#include "motion_state.hpp"
#include "wave_generator.hpp"

namespace app_modules {
namespace {

constexpr char kTag[] = "ui";
constexpr uint32_t kUiUpdateMs = 16;
constexpr uint32_t kTransitionMs = 460;
constexpr uint32_t kCarouselDurationMs = 520;
constexpr uint32_t kCarouselBootDurationMs = 800;
constexpr TickType_t kLongPressTicks = pdMS_TO_TICKS(650);
constexpr std::array<uint32_t, 5> kAccentHex = {
    0x67E8F9,
    0xA78BFA,
    0xFB7185,
    0x34D399,
    0xFBBF24,
};

lv_color_t accent(size_t index)
{
    return lv_color_hex(kAccentHex[index % kAccentHex.size()]);
}

enum class Page : uint8_t {
    menu,
    motion,
    ambient,
    system,
    wave,
    arcade,
    game,
};

using AnimExec = void (*)(void*, int32_t);

void anim_x(void* object, int32_t value)
{
    lv_obj_set_x(static_cast<lv_obj_t*>(object), value);
}

void anim_y(void* object, int32_t value)
{
    lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
}

void anim_opa(void* object, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void anim_scale(void* object, int32_t value)
{
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(object), value, 0);
}

void anim_border_opa(void* object, int32_t value)
{
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(value), 0);
}

void anim_arc_rotation(void* object, int32_t value)
{
    lv_obj_t* arc = static_cast<lv_obj_t*>(object);
    const int32_t rotation = value / 10;
    if (lv_arc_get_rotation(arc) != rotation) {
        lv_arc_set_rotation(arc, rotation);
    }
}

void start_animation(void* object,
                     AnimExec callback,
                     int32_t from,
                     int32_t to,
                     uint32_t duration,
                     uint32_t delay = 0,
                     lv_anim_path_cb_t path = lv_anim_path_ease_out)
{
    lv_anim_delete(object, callback);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_path_cb(&animation, path);
    lv_anim_start(&animation);
}

void start_loop_animation(void* object,
                          AnimExec callback,
                          int32_t from,
                          int32_t to,
                          uint32_t duration,
                          uint32_t reverse_duration,
                          uint32_t delay = 0)
{
    lv_anim_delete(object, callback);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_reverse_duration(&animation, reverse_duration);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

void style_screen(lv_obj_t* screen, lv_color_t left, lv_color_t right)
{
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, left, 0);
    lv_obj_set_style_bg_grad_color(screen, right, 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
}

lv_obj_t* create_label(lv_obj_t* parent,
                       const char* text,
                       const lv_font_t* font,
                       lv_color_t color)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

struct Fixed1 {
    int32_t scaled;
    char sign;
    uint32_t whole;
    uint32_t fraction;
};

Fixed1 to_fixed_1(float value)
{
    const int32_t scaled = static_cast<int32_t>(std::lround(value * 10.0f));
    const uint32_t magnitude =
        scaled < 0 ? static_cast<uint32_t>(-scaled) : static_cast<uint32_t>(scaled);
    return {
        .scaled = scaled,
        .sign = scaled < 0 ? '-' : '+',
        .whole = magnitude / 10,
        .fraction = magnitude % 10,
    };
}

lv_obj_t* create_glass_panel(lv_obj_t* parent, int x, int y, int width, int height)
{
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x17233D), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x395071), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

void set_visible(lv_obj_t* object, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

class UiController {
public:
    bool create()
    {
        create_menu_screen();
        create_motion_screen();
        create_ambient_screen();
        create_system_screen();
        create_wave_screen();
        create_arcade_screen();
        create_game_screen();

        lv_screen_load(menu_screen_);
        current_page_ = Page::menu;
        update_carousel(true);
        update_timer_ = lv_timer_create(timer_callback, kUiUpdateMs, this);
        if (!update_timer_) {
            destroy();
            return false;
        }
#ifdef M5_STICKS3_HW_SMOKE_TEST
        start_hardware_smoke_test();
#endif
        return true;
    }

    void destroy()
    {
        request_motion_enabled(false);
        auto& generator = wave::Generator::instance();
        if (generator.initialized() && generator.enabled()) {
            const esp_err_t result = generator.set_enabled(false);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "failed to mute wave outputs during UI teardown: %s",
                         esp_err_to_name(result));
            }
        }
        if (carousel_timer_) {
            lv_timer_delete(carousel_timer_);
            carousel_timer_ = nullptr;
        }
        if (update_timer_) {
            lv_timer_delete(update_timer_);
            update_timer_ = nullptr;
        }
#ifdef M5_STICKS3_HW_SMOKE_TEST
        if (smoke_timer_) {
            lv_timer_delete(smoke_timer_);
            smoke_timer_ = nullptr;
        }
#endif
        for (lv_obj_t*& screen : screens_) {
            if (screen) {
                lv_obj_delete(screen);
                screen = nullptr;
            }
        }
        menu_screen_ = nullptr;
        motion_screen_ = nullptr;
        ambient_screen_ = nullptr;
        system_screen_ = nullptr;
        wave_screen_ = nullptr;
        arcade_screen_ = nullptr;
        game_screen_ = nullptr;
    }

    void key1()
    {
        if (current_page_ == Page::menu) {
            if (carousel_busy_) {
                carousel_step_queued_ = true;
                return;
            }
            advance_carousel();
        } else if (current_page_ == Page::arcade) {
            advance_arcade();
        } else if (current_page_ == Page::game) {
            show_arcade();
        } else {
            show_menu();
        }
    }

    void key2_pressed()
    {
        if (current_page_ != Page::game || game_waiting_for_motion_) {
            return;
        }

        if (active_game_ == mini_games::GameId::meteor_dodge) {
            if (meteor_dodge_.activate_shield()) {
                lv_label_set_text(game_status_label_, "SHIELD ACTIVE");
                start_animation(game_status_label_, anim_opa, LV_OPA_COVER, LV_OPA_50, 520);
            }
        } else if (active_game_ == mini_games::GameId::tap_runner) {
            tap_runner_.jump();
        }
    }

    void key2(bool long_press)
    {
        switch (current_page_) {
        case Page::menu:
            if (!carousel_busy_) {
                open_selected();
            }
            break;
        case Page::motion: {
            const model::MotionSample sample = model::MotionState::instance().snapshot();
            if (!sample.valid) {
                lv_label_set_text(motion_action_label_, "WAIT FOR IMU");
                start_animation(motion_action_label_, anim_opa, LV_OPA_COVER, LV_OPA_40,
                                700, 250, lv_anim_path_ease_out);
                break;
            }
            yaw_zero_ = sample.yaw_deg;
            lv_label_set_text(motion_action_label_, "YAW ZEROED");
            start_animation(motion_action_label_, anim_opa, LV_OPA_COVER, LV_OPA_40, 900, 350,
                            lv_anim_path_ease_out);
            break;
        }
        case Page::ambient:
            ambient_palette_ = (ambient_palette_ + 1) % kAccentHex.size();
            lv_obj_set_style_bg_color(ambient_orb_, accent(ambient_palette_), 0);
            lv_obj_set_style_arc_color(ambient_arc_a_, accent(ambient_palette_), LV_PART_INDICATOR);
            break;
        case Page::wave:
            if (long_press) {
                advance_wave_duty();
            } else {
                advance_wave_frequency();
            }
            break;
        case Page::system:
            break;
        case Page::arcade:
            if (long_press) {
                show_menu();
            } else {
                open_arcade_game();
            }
            break;
        case Page::game:
            if (long_press) {
                restart_current_game();
            } else if (current_game_state() != mini_games::RunState::playing) {
                restart_current_game();
            } else if (active_game_ == mini_games::GameId::tilt_quest) {
                const model::MotionSample sample = model::MotionState::instance().snapshot();
                if (sample.valid) {
                    calibrate_game_motion(sample);
                    lv_label_set_text(game_status_label_, "CENTERED");
                    start_animation(game_status_label_, anim_opa, LV_OPA_COVER, LV_OPA_50,
                                    520);
                }
            }
            break;
        }
    }

private:
    static void timer_callback(lv_timer_t* timer)
    {
        static_cast<UiController*>(lv_timer_get_user_data(timer))->update();
    }

    static void carousel_timer_callback(lv_timer_t* timer)
    {
        auto* controller = static_cast<UiController*>(lv_timer_get_user_data(timer));
        controller->carousel_timer_ = nullptr;
        lv_timer_delete(timer);
        controller->carousel_busy_ = false;

        const bool advance_queued_step =
            controller->carousel_step_queued_ && controller->current_page_ == Page::menu;
        controller->carousel_step_queued_ = false;
        if (advance_queued_step) {
            controller->advance_carousel();
        }
    }

#ifdef M5_STICKS3_HW_SMOKE_TEST
    static void smoke_timer_callback(lv_timer_t* timer)
    {
        static_cast<UiController*>(lv_timer_get_user_data(timer))->
            advance_hardware_smoke_test();
    }

    void start_hardware_smoke_test()
    {
        active_game_ = mini_games::GameId::tilt_quest;
        current_page_ = Page::game;
        configure_game_objects();
        restart_current_game();
        lv_screen_load(game_screen_);
        smoke_step_ = 0;
        smoke_timer_ = lv_timer_create(smoke_timer_callback, 4000, this);
        ESP_LOGI(kTag, "HW smoke 1/3: TILT QUEST");
    }

    void advance_hardware_smoke_test()
    {
        ++smoke_step_;
        if (smoke_step_ == 1) {
            active_game_ = mini_games::GameId::meteor_dodge;
            configure_game_objects();
            restart_current_game();
            ESP_LOGI(kTag, "HW smoke 2/3: METEOR DODGE");
            return;
        }
        if (smoke_step_ == 2) {
            active_game_ = mini_games::GameId::tap_runner;
            configure_game_objects();
            restart_current_game();
            tap_runner_.jump();
            ESP_LOGI(kTag, "HW smoke 3/3: TAP RUNNER");
            return;
        }

        request_motion_enabled(false);
        current_page_ = Page::menu;
        lv_screen_load(menu_screen_);
        lv_timer_delete(smoke_timer_);
        smoke_timer_ = nullptr;
        ESP_LOGI(kTag, "HW smoke complete: all game pages exercised");
    }
#endif

    void update()
    {
        if (current_page_ == Page::motion) {
            update_motion();
        } else if (current_page_ == Page::system) {
            update_system();
        } else if (current_page_ == Page::game) {
            update_game();
        }
    }

    void create_menu_screen()
    {
        menu_screen_ = lv_obj_create(nullptr);
        screens_[0] = menu_screen_;
        style_screen(menu_screen_, lv_color_hex(0x07111F), lv_color_hex(0x15102A));

        lv_obj_t* bloom_a = lv_obj_create(menu_screen_);
        lv_obj_set_size(bloom_a, 100, 100);
        lv_obj_set_pos(bloom_a, -48, -43);
        lv_obj_set_style_radius(bloom_a, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bloom_a, accent(0), 0);
        lv_obj_set_style_bg_opa(bloom_a, LV_OPA_10, 0);
        lv_obj_set_style_border_width(bloom_a, 0, 0);

        lv_obj_t* bloom_b = lv_obj_create(menu_screen_);
        lv_obj_set_size(bloom_b, 92, 92);
        lv_obj_set_pos(bloom_b, 75, 166);
        lv_obj_set_style_radius(bloom_b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bloom_b, accent(1), 0);
        lv_obj_set_style_bg_opa(bloom_b, LV_OPA_10, 0);
        lv_obj_set_style_border_width(bloom_b, 0, 0);

        lv_obj_t* brand = create_label(menu_screen_, "STICKS3 / MOTION", &lv_font_montserrat_12,
                                       lv_color_hex(0x8292AE));
        lv_obj_set_pos(brand, 8, 9);

        lv_obj_t* dot = lv_obj_create(menu_screen_);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, 120, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, accent(0), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_shadow_color(dot, accent(0), 0);
        lv_obj_set_style_shadow_opa(dot, LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(dot, 10, 0);

        selector_glow_ = lv_obj_create(menu_screen_);
        lv_obj_set_pos(selector_glow_, 31, 65);
        lv_obj_set_size(selector_glow_, 72, 78);
        lv_obj_set_style_radius(selector_glow_, 17, 0);
        lv_obj_set_style_bg_opa(selector_glow_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(selector_glow_, 2, 0);
        lv_obj_set_style_border_color(selector_glow_, accent(0), 0);
        lv_obj_set_style_border_opa(selector_glow_, LV_OPA_40, 0);
        start_loop_animation(selector_glow_, anim_border_opa, LV_OPA_20, LV_OPA_70,
                             1100, 1100);

        constexpr std::array<const char*, 5> symbols = {
            LV_SYMBOL_GPS, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_SETTINGS,
            LV_SYMBOL_SHUFFLE, LV_SYMBOL_PLAY};
        constexpr std::array<const char*, 5> titles = {
            "MOTION", "AURA", "SYSTEM", "WAVE", "ARCADE"};
        for (size_t index = 0; index < cards_.size(); ++index) {
            lv_obj_t* card = create_glass_panel(menu_screen_, 31, 148, 72, 72);
            cards_[index] = card;
            lv_obj_set_style_transform_pivot_x(card, 36, 0);
            lv_obj_set_style_transform_pivot_y(card, 36, 0);
            lv_obj_set_style_border_color(card, accent(index), 0);
            lv_obj_set_style_shadow_color(card, accent(index), 0);
            lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
            lv_obj_set_style_shadow_width(card, 8, 0);

            lv_obj_t* icon = create_label(card, symbols[index], &lv_font_montserrat_24, accent(index));
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
            lv_obj_t* title = create_label(card, titles[index], &lv_font_montserrat_12,
                                           lv_color_hex(0xE8EEF8));
            lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -9);
        }

        menu_hint_ = create_label(menu_screen_, "100 Hz VQF orientation", &lv_font_montserrat_12,
                                  lv_color_hex(0x95A4BD));
        lv_obj_set_width(menu_hint_, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(menu_hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(menu_hint_, 0, 174);

        lv_obj_t* controls = create_label(menu_screen_, "K1 NEXT   K2 OPEN",
                                          &lv_font_montserrat_12, lv_color_hex(0x65758F));
        lv_obj_set_width(controls, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(controls, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(controls, 0, 219);
    }

    void create_motion_screen()
    {
        motion_screen_ = lv_obj_create(nullptr);
        screens_[1] = motion_screen_;
        style_screen(motion_screen_, lv_color_hex(0x06131E), lv_color_hex(0x101B35));

        lv_obj_t* title = create_label(motion_screen_, "ATTITUDE / DEG", &lv_font_montserrat_14,
                                       lv_color_hex(0xEEF8FF));
        lv_obj_set_pos(title, 8, 7);
        motion_status_label_ = create_label(motion_screen_, "BMI270 / SLEEP",
                                            &lv_font_montserrat_12, lv_color_hex(0x73859B));
        lv_obj_set_pos(motion_status_label_, 8, 29);

        lv_obj_t* attitude_panel = create_glass_panel(motion_screen_, 23, 49, 88, 88);
        lv_obj_set_style_radius(attitude_panel, 20, 0);
        attitude_clip_ = lv_obj_create(attitude_panel);
        lv_obj_set_pos(attitude_clip_, 7, 7);
        lv_obj_set_size(attitude_clip_, 74, 74);
        lv_obj_remove_flag(attitude_clip_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(attitude_clip_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(attitude_clip_, lv_color_hex(0x0A1C2A), 0);
        lv_obj_set_style_bg_opa(attitude_clip_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(attitude_clip_, 2, 0);
        lv_obj_set_style_border_color(attitude_clip_, accent(0), 0);
        lv_obj_set_style_border_opa(attitude_clip_, LV_OPA_60, 0);
        lv_obj_set_style_pad_all(attitude_clip_, 0, 0);

        attitude_sky_ = lv_obj_create(attitude_clip_);
        lv_obj_set_pos(attitude_sky_, 5, 8);
        lv_obj_set_size(attitude_sky_, 64, 29);
        lv_obj_set_style_bg_color(attitude_sky_, lv_color_hex(0x174A66), 0);
        lv_obj_set_style_bg_opa(attitude_sky_, LV_OPA_70, 0);
        lv_obj_set_style_border_width(attitude_sky_, 0, 0);
        lv_obj_set_style_radius(attitude_sky_, 5, 0);

        horizon_line_ = lv_obj_create(attitude_clip_);
        lv_obj_set_pos(horizon_line_, 6, 36);
        lv_obj_set_size(horizon_line_, 62, 3);
        lv_obj_set_style_radius(horizon_line_, 2, 0);
        lv_obj_set_style_bg_color(horizon_line_, accent(0), 0);
        lv_obj_set_style_border_width(horizon_line_, 0, 0);
        lv_obj_set_style_transform_pivot_x(horizon_line_, 31, 0);
        lv_obj_set_style_transform_pivot_y(horizon_line_, 1, 0);
        lv_obj_set_style_shadow_color(horizon_line_, accent(0), 0);
        lv_obj_set_style_shadow_width(horizon_line_, 6, 0);
        lv_obj_set_style_shadow_opa(horizon_line_, LV_OPA_50, 0);

        lv_obj_t* reticle = lv_obj_create(attitude_clip_);
        lv_obj_set_size(reticle, 7, 7);
        lv_obj_center(reticle);
        lv_obj_set_style_radius(reticle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(reticle, lv_color_hex(0xF7FAFC), 0);
        lv_obj_set_style_border_width(reticle, 0, 0);

        lv_obj_t* values = create_glass_panel(motion_screen_, 8, 144, 119, 72);
        angles_label_ = create_label(values, "ROLL   +0.0\nPITCH  +0.0\nYAW    +0.0",
                                     &lv_font_montserrat_14, lv_color_hex(0xF1F5F9));
        lv_obj_set_pos(angles_label_, 7, 4);
        lv_obj_set_style_text_line_space(angles_label_, 3, 0);
        raw_label_ = create_label(values, "a  +0.00 +0.00 +0.00", &lv_font_montserrat_12,
                                  lv_color_hex(0x8FA2BD));
        lv_obj_set_pos(raw_label_, 7, 55);

        motion_action_label_ = create_label(motion_screen_, "K1 BACK   K2 ZERO YAW",
                                            &lv_font_montserrat_12, lv_color_hex(0x7688A4));
        lv_obj_set_width(motion_action_label_, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(motion_action_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(motion_action_label_, 0, 222);

        lv_obj_set_style_transform_scale(attitude_panel, 256, 0);
    }

    void create_ambient_screen()
    {
        ambient_screen_ = lv_obj_create(nullptr);
        screens_[2] = ambient_screen_;
        style_screen(ambient_screen_, lv_color_hex(0x0A0B1D), lv_color_hex(0x241339));

        lv_obj_t* title = create_label(ambient_screen_, "AURA / SLOW MOTION", &lv_font_montserrat_12,
                                       lv_color_hex(0xA9B4CA));
        lv_obj_set_pos(title, 8, 9);

        lv_obj_t* ambient_halo = lv_obj_create(ambient_screen_);
        lv_obj_set_size(ambient_halo, 98, 98);
        lv_obj_align(ambient_halo, LV_ALIGN_CENTER, 0, -2);
        lv_obj_set_style_radius(ambient_halo, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ambient_halo, accent(1), 0);
        lv_obj_set_style_bg_opa(ambient_halo, LV_OPA_20, 0);
        lv_obj_set_style_border_width(ambient_halo, 0, 0);

        ambient_orb_ = lv_obj_create(ambient_screen_);
        lv_obj_set_size(ambient_orb_, 62, 62);
        lv_obj_align(ambient_orb_, LV_ALIGN_CENTER, 0, -2);
        lv_obj_set_style_radius(ambient_orb_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ambient_orb_, accent(1), 0);
        lv_obj_set_style_bg_grad_color(ambient_orb_, accent(0), 0);
        lv_obj_set_style_bg_grad_dir(ambient_orb_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(ambient_orb_, 0, 0);
        lv_obj_set_style_transform_scale(ambient_orb_, 256, 0);

        ambient_arc_a_ = lv_arc_create(ambient_screen_);
        lv_obj_set_size(ambient_arc_a_, 104, 104);
        lv_obj_align(ambient_arc_a_, LV_ALIGN_CENTER, 0, -2);
        lv_arc_set_bg_angles(ambient_arc_a_, 0, 360);
        lv_arc_set_range(ambient_arc_a_, 0, 100);
        lv_arc_set_value(ambient_arc_a_, 67);
        lv_obj_remove_style(ambient_arc_a_, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_width(ambient_arc_a_, 1, LV_PART_MAIN);
        lv_obj_set_style_arc_color(ambient_arc_a_, lv_color_hex(0x34415D), LV_PART_MAIN);
        lv_obj_set_style_arc_width(ambient_arc_a_, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ambient_arc_a_, accent(1), LV_PART_INDICATOR);
        start_loop_animation(ambient_arc_a_, anim_arc_rotation, 0, 3600, 5200, 0);

        lv_obj_t* arc_b = lv_arc_create(ambient_screen_);
        lv_obj_set_size(arc_b, 122, 122);
        lv_obj_align(arc_b, LV_ALIGN_CENTER, 0, -2);
        lv_arc_set_bg_angles(arc_b, 0, 360);
        lv_arc_set_range(arc_b, 0, 100);
        lv_arc_set_value(arc_b, 38);
        lv_obj_remove_style(arc_b, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc_b, 1, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc_b, lv_color_hex(0x2D3650), LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc_b, 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc_b, accent(0), LV_PART_INDICATOR);
        start_loop_animation(arc_b, anim_arc_rotation, 3600, 0, 7600, 0);

        for (size_t index = 0; index < ambient_particles_.size(); ++index) {
            lv_obj_t* particle = lv_obj_create(ambient_screen_);
            ambient_particles_[index] = particle;
            constexpr std::array<int, 6> x_positions = {11, 29, 48, 83, 104, 120};
            const int x = x_positions[index];
            const int y = 52 + static_cast<int>((index * 31) % 130);
            lv_obj_set_pos(particle, x, y);
            lv_obj_set_size(particle, 4 + (index % 2) * 2, 4 + (index % 2) * 2);
            lv_obj_set_style_radius(particle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(particle, accent(index % kAccentHex.size()), 0);
            lv_obj_set_style_bg_opa(particle, LV_OPA_60, 0);
            lv_obj_set_style_border_width(particle, 0, 0);
            start_loop_animation(particle, anim_y, y - 5, y + 8, 1200 + index * 170,
                                 1200 + index * 170, index * 90);
        }

        lv_obj_t* hint = create_label(ambient_screen_, "K1 BACK       K2 COLOR", &lv_font_montserrat_12,
                                      lv_color_hex(0x7D8BA4));
        lv_obj_set_width(hint, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 220);
    }

    void create_system_screen()
    {
        system_screen_ = lv_obj_create(nullptr);
        screens_[3] = system_screen_;
        style_screen(system_screen_, lv_color_hex(0x08121A), lv_color_hex(0x142432));

        lv_obj_t* title = create_label(system_screen_, "SYSTEM", &lv_font_montserrat_14,
                                       lv_color_hex(0xF1F5F9));
        lv_obj_set_pos(title, 8, 8);
        lv_obj_t* badge = create_label(system_screen_, "ESP-IDF 5.5", &lv_font_montserrat_12,
                                       accent(0));
        lv_obj_set_pos(badge, 8, 30);

        lv_obj_t* identity = create_glass_panel(system_screen_, 8, 53, 119, 67);
        lv_obj_t* chip = create_label(identity, "ESP32-S3", &lv_font_montserrat_18,
                                      lv_color_hex(0xF8FAFC));
        lv_obj_set_pos(chip, 7, 7);
        lv_obj_t* spec = create_label(identity, "240 MHz dual core\n8 MB flash / OPI RAM",
                                      &lv_font_montserrat_12, lv_color_hex(0x92A4BD));
        lv_obj_set_pos(spec, 7, 34);

        lv_obj_t* telemetry = create_glass_panel(system_screen_, 8, 128, 119, 79);
        heap_label_ = create_label(telemetry, "HEAP  -- KB", &lv_font_montserrat_14,
                                   lv_color_hex(0xE2E8F0));
        psram_label_ = create_label(telemetry, "PSRAM -- KB", &lv_font_montserrat_14,
                                    lv_color_hex(0xE2E8F0));
        uptime_label_ = create_label(telemetry, "UP    -- s", &lv_font_montserrat_14,
                                     lv_color_hex(0xE2E8F0));
        lv_obj_set_pos(heap_label_, 8, 9);
        lv_obj_set_pos(psram_label_, 8, 31);
        lv_obj_set_pos(uptime_label_, 8, 53);

        lv_obj_t* hint = create_label(system_screen_, "K1 BACK", &lv_font_montserrat_12,
                                      lv_color_hex(0x7D8BA4));
        lv_obj_set_width(hint, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 220);
    }

    void create_wave_screen()
    {
        wave_screen_ = lv_obj_create(nullptr);
        screens_[4] = wave_screen_;
        style_screen(wave_screen_, lv_color_hex(0x041512), lv_color_hex(0x10233A));

        lv_obj_t* glow = lv_obj_create(wave_screen_);
        lv_obj_set_size(glow, 104, 104);
        lv_obj_set_pos(glow, 73, -47);
        lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(glow, accent(3), 0);
        lv_obj_set_style_bg_opa(glow, LV_OPA_10, 0);
        lv_obj_set_style_border_width(glow, 0, 0);

        lv_obj_t* title = create_label(wave_screen_, "SIGNAL LAB",
                                       &lv_font_montserrat_12, lv_color_hex(0xDDFBF2));
        lv_obj_set_pos(title, 8, 8);
        wave_status_dot_ = lv_obj_create(wave_screen_);
        lv_obj_set_pos(wave_status_dot_, 98, 11);
        lv_obj_set_size(wave_status_dot_, 6, 6);
        lv_obj_set_style_radius(wave_status_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(wave_status_dot_, 0, 0);
        wave_status_label_ = create_label(wave_screen_, "OFF", &lv_font_montserrat_12,
                                          lv_color_hex(0x73859B));
        lv_obj_set_pos(wave_status_label_, 108, 7);

        wave_duty_label_ = create_label(wave_screen_, "DUTY 50% / 2-BIT",
                                        &lv_font_montserrat_12, lv_color_hex(0x6D8791));
        lv_obj_set_pos(wave_duty_label_, 8, 29);

        wave_scope_ = create_glass_panel(wave_screen_, 8, 49, 119, 94);
        lv_obj_set_style_border_color(wave_scope_, accent(3), 0);
        lv_obj_set_style_border_opa(wave_scope_, LV_OPA_40, 0);
        wave_frequency_label_ = create_label(wave_scope_, "1.000", &lv_font_montserrat_24,
                                              lv_color_hex(0xF0FFF9));
        lv_obj_set_pos(wave_frequency_label_, 8, 6);
        wave_unit_label_ = create_label(wave_scope_, "MHz", &lv_font_montserrat_12,
                                        accent(3));
        lv_obj_set_pos(wave_unit_label_, 83, 15);

        constexpr std::array<int, 9> segment_x = {8, 27, 27, 50, 50, 73, 73, 96, 96};
        constexpr std::array<int, 9> segment_y = {62, 35, 35, 35, 62, 35, 35, 35, 62};
        constexpr std::array<int, 9> segment_w = {19, 2, 23, 2, 23, 2, 23, 2, 15};
        constexpr std::array<int, 9> segment_h = {2, 29, 2, 29, 2, 29, 2, 29, 2};
        for (size_t index = 0; index < wave_segments_.size(); ++index) {
            lv_obj_t* segment = lv_obj_create(wave_scope_);
            wave_segments_[index] = segment;
            lv_obj_set_pos(segment, segment_x[index], segment_y[index]);
            lv_obj_set_size(segment, segment_w[index], segment_h[index]);
            lv_obj_set_style_radius(segment, 2, 0);
            lv_obj_set_style_bg_color(segment, accent(3), 0);
            lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(segment, 0, 0);
            lv_obj_set_style_shadow_color(segment, accent(3), 0);
            lv_obj_set_style_shadow_width(segment, 5, 0);
            lv_obj_set_style_shadow_opa(segment, LV_OPA_40, 0);
        }

        lv_obj_t* output_panel = create_glass_panel(wave_screen_, 8, 151, 119, 51);
        lv_obj_t* output_title = create_label(output_panel, "OUTPUT BUS", &lv_font_montserrat_12,
                                               lv_color_hex(0x71879A));
        lv_obj_set_pos(output_title, 7, 3);
        constexpr std::array<const char*, 3> output_names = {"G4", "G5", "5V"};
        constexpr std::array<int, 3> output_x = {7, 43, 79};
        for (size_t index = 0; index < output_names.size(); ++index) {
            lv_obj_t* chip = create_glass_panel(output_panel, output_x[index], 23, 31, 20);
            lv_obj_set_style_radius(chip, 10, 0);
            lv_obj_set_style_bg_color(chip, index == 2 ? lv_color_hex(0x16352B)
                                                       : lv_color_hex(0x12312E), 0);
            lv_obj_set_style_border_color(chip, accent(3), 0);
            lv_obj_set_style_border_opa(chip, index == 2 ? LV_OPA_40 : LV_OPA_70, 0);
            lv_obj_t* label = create_label(chip, output_names[index], &lv_font_montserrat_12,
                                           index == 2 ? lv_color_hex(0xA7C9BC) : accent(3));
            lv_obj_center(label);
        }

        lv_obj_t* hint = create_label(wave_screen_, "K1 BACK  K2 F/HOLD D", &lv_font_montserrat_12,
                                      lv_color_hex(0x708799));
        lv_obj_set_width(hint, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 220);

        update_wave_display();
    }

    void create_arcade_screen()
    {
        arcade_screen_ = lv_obj_create(nullptr);
        screens_[5] = arcade_screen_;
        style_screen(arcade_screen_, lv_color_hex(0x120C08), lv_color_hex(0x25112A));

        lv_obj_t* title = create_label(arcade_screen_, "POCKET ARCADE",
                                       &lv_font_montserrat_14, lv_color_hex(0xFFF7ED));
        lv_obj_set_pos(title, 8, 8);
        lv_obj_t* badge = create_label(arcade_screen_, "3 MICRO GAMES",
                                       &lv_font_montserrat_12, accent(4));
        lv_obj_set_pos(badge, 8, 29);

        arcade_panel_ = create_glass_panel(arcade_screen_, 8, 50, 119, 143);
        lv_obj_set_style_border_color(arcade_panel_, accent(4), 0);
        lv_obj_set_style_transform_pivot_x(arcade_panel_, 59, 0);
        lv_obj_set_style_transform_pivot_y(arcade_panel_, 71, 0);

        arcade_index_label_ = create_label(arcade_panel_, "01 / 03",
                                           &lv_font_montserrat_12,
                                           lv_color_hex(0x8796AD));
        lv_obj_set_pos(arcade_index_label_, 8, 7);
        arcade_icon_ = create_label(arcade_panel_, LV_SYMBOL_GPS,
                                    &lv_font_montserrat_24, accent(4));
        lv_obj_align(arcade_icon_, LV_ALIGN_TOP_MID, 0, 27);
        arcade_name_label_ = create_label(arcade_panel_, "TILT QUEST",
                                          &lv_font_montserrat_16,
                                          lv_color_hex(0xFFF7ED));
        lv_obj_set_width(arcade_name_label_, 119);
        lv_obj_set_style_text_align(arcade_name_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(arcade_name_label_, 0, 61);
        arcade_description_label_ = create_label(
            arcade_panel_, "Tilt through the maze\nand collect 5 beacons",
            &lv_font_montserrat_12, lv_color_hex(0xA8B3C7));
        lv_obj_set_width(arcade_description_label_, 107);
        lv_obj_set_style_text_align(arcade_description_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(arcade_description_label_, 6, 87);

        for (size_t index = 0; index < arcade_dots_.size(); ++index) {
            lv_obj_t* dot = lv_obj_create(arcade_panel_);
            arcade_dots_[index] = dot;
            lv_obj_set_pos(dot, 45 + static_cast<int>(index) * 13, 127);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
        }

        lv_obj_t* controls = create_label(arcade_screen_, "K1 NEXT   K2 PLAY\nHOLD K2: MAIN MENU",
                                          &lv_font_montserrat_12,
                                          lv_color_hex(0x766E82));
        lv_obj_set_width(controls, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(controls, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(controls, 0, 0);
        lv_obj_set_pos(controls, 0, 209);
        update_arcade_selector(true);
    }

    void create_game_screen()
    {
        game_screen_ = lv_obj_create(nullptr);
        screens_[6] = game_screen_;
        style_screen(game_screen_, lv_color_hex(0x070C16), lv_color_hex(0x151023));

        game_title_label_ = create_label(game_screen_, "TILT QUEST",
                                         &lv_font_montserrat_14,
                                         lv_color_hex(0xF8FAFC));
        lv_obj_set_pos(game_title_label_, 8, 7);
        game_score_label_ = create_label(game_screen_, "0 / 5",
                                         &lv_font_montserrat_14, accent(4));
        lv_obj_set_width(game_score_label_, 65);
        lv_obj_set_style_text_align(game_score_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(game_score_label_, 62, 7);
        game_status_label_ = create_label(game_screen_, "READY",
                                          &lv_font_montserrat_12,
                                          lv_color_hex(0x8392A8));
        lv_obj_set_pos(game_status_label_, 8, 27);

        game_field_ = create_glass_panel(game_screen_, 8, 43, 119, 164);
        lv_obj_set_style_radius(game_field_, 12, 0);
        lv_obj_set_style_bg_color(game_field_, lv_color_hex(0x071421), 0);
        lv_obj_set_style_bg_opa(game_field_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(game_field_, accent(4), 0);

        tilt_quest_.reset(1);
        tilt_goal_ = lv_obj_create(game_field_);
        lv_obj_set_size(tilt_goal_, 15, 15);
        lv_obj_set_style_radius(tilt_goal_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(tilt_goal_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tilt_goal_, 3, 0);
        lv_obj_set_style_border_color(tilt_goal_, accent(4), 0);
        lv_obj_set_style_shadow_color(tilt_goal_, accent(4), 0);
        lv_obj_set_style_shadow_width(tilt_goal_, 7, 0);
        lv_obj_set_style_shadow_opa(tilt_goal_, LV_OPA_50, 0);

        for (size_t index = 0; index < tilt_walls_.size(); ++index) {
            const mini_games::Rect& wall = tilt_quest_.walls()[index];
            lv_obj_t* object = lv_obj_create(game_field_);
            tilt_walls_[index] = object;
            lv_obj_set_pos(object, 2 + static_cast<int>(wall.x),
                           2 + static_cast<int>(wall.y));
            lv_obj_set_size(object, static_cast<int>(wall.width),
                            static_cast<int>(wall.height));
            lv_obj_set_style_radius(object, 4, 0);
            lv_obj_set_style_bg_color(object, lv_color_hex(0x30405A), 0);
            lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(object, 0, 0);
        }

        tilt_ball_ = lv_obj_create(game_field_);
        lv_obj_set_size(tilt_ball_, 10, 10);
        lv_obj_set_style_radius(tilt_ball_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(tilt_ball_, lv_color_hex(0xF8FAFC), 0);
        lv_obj_set_style_border_width(tilt_ball_, 0, 0);
        lv_obj_set_style_shadow_color(tilt_ball_, accent(0), 0);
        lv_obj_set_style_shadow_width(tilt_ball_, 8, 0);
        lv_obj_set_style_shadow_opa(tilt_ball_, LV_OPA_70, 0);

        meteor_player_ = lv_obj_create(game_field_);
        lv_obj_set_size(meteor_player_, 13, 11);
        lv_obj_set_style_radius(meteor_player_, 4, 0);
        lv_obj_set_style_bg_color(meteor_player_, accent(4), 0);
        lv_obj_set_style_border_width(meteor_player_, 1, 0);
        lv_obj_set_style_border_color(meteor_player_, lv_color_hex(0xFFF7D6), 0);

        meteor_shield_ = lv_obj_create(game_field_);
        lv_obj_set_size(meteor_shield_, 22, 22);
        lv_obj_set_style_radius(meteor_shield_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(meteor_shield_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(meteor_shield_, 2, 0);
        lv_obj_set_style_border_color(meteor_shield_, accent(0), 0);
        lv_obj_set_style_shadow_color(meteor_shield_, accent(0), 0);
        lv_obj_set_style_shadow_width(meteor_shield_, 8, 0);
        lv_obj_set_style_shadow_opa(meteor_shield_, LV_OPA_70, 0);

        for (size_t index = 0; index < meteor_objects_.size(); ++index) {
            lv_obj_t* meteor = lv_obj_create(game_field_);
            meteor_objects_[index] = meteor;
            lv_obj_set_size(meteor, 9, 9);
            lv_obj_set_style_radius(meteor, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(meteor, index % 2 == 0 ? accent(2)
                                                             : lv_color_hex(0xF97316), 0);
            lv_obj_set_style_border_width(meteor, 0, 0);
            lv_obj_set_style_shadow_color(meteor, accent(2), 0);
            lv_obj_set_style_shadow_width(meteor, 4, 0);
            lv_obj_set_style_shadow_opa(meteor, LV_OPA_40, 0);
        }

        runner_ground_ = lv_obj_create(game_field_);
        lv_obj_set_pos(runner_ground_, 2, 2 + static_cast<int>(mini_games::TapRunner::kGroundY));
        lv_obj_set_size(runner_ground_, static_cast<int>(mini_games::kFieldWidth), 2);
        lv_obj_set_style_radius(runner_ground_, 1, 0);
        lv_obj_set_style_bg_color(runner_ground_, accent(3), 0);
        lv_obj_set_style_border_width(runner_ground_, 0, 0);

        runner_player_ = lv_obj_create(game_field_);
        lv_obj_set_size(runner_player_, static_cast<int>(mini_games::TapRunner::kPlayerSize),
                        static_cast<int>(mini_games::TapRunner::kPlayerSize));
        lv_obj_set_style_radius(runner_player_, 3, 0);
        lv_obj_set_style_bg_color(runner_player_, accent(3), 0);
        lv_obj_set_style_border_width(runner_player_, 0, 0);
        lv_obj_set_style_shadow_color(runner_player_, accent(3), 0);
        lv_obj_set_style_shadow_width(runner_player_, 6, 0);
        lv_obj_set_style_shadow_opa(runner_player_, LV_OPA_50, 0);

        runner_obstacle_ = lv_obj_create(game_field_);
        lv_obj_set_style_radius(runner_obstacle_, 3, 0);
        lv_obj_set_style_bg_color(runner_obstacle_, accent(2), 0);
        lv_obj_set_style_border_width(runner_obstacle_, 0, 0);

        game_overlay_ = create_glass_panel(game_field_, 11, 52, 93, 58);
        lv_obj_set_style_bg_color(game_overlay_, lv_color_hex(0x111827), 0);
        lv_obj_set_style_bg_opa(game_overlay_, LV_OPA_90, 0);
        game_overlay_label_ = create_label(game_overlay_, "READY",
                                           &lv_font_montserrat_14,
                                           lv_color_hex(0xF8FAFC));
        lv_obj_set_width(game_overlay_label_, 93);
        lv_obj_set_style_text_align(game_overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(game_overlay_label_);

        game_controls_label_ = create_label(game_screen_, "K1 BACK  K2 CENTER",
                                            &lv_font_montserrat_12,
                                            lv_color_hex(0x738096));
        lv_obj_set_width(game_controls_label_, bsp::kDisplayWidth);
        lv_obj_set_style_text_align(game_controls_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(game_controls_label_, 0, 220);
        configure_game_objects();
    }

    void advance_arcade()
    {
        arcade_selected_ = (arcade_selected_ + 1) % 3;
        update_arcade_selector(false);
    }

    void update_arcade_selector(bool boot)
    {
        constexpr std::array<const char*, 3> symbols = {
            LV_SYMBOL_GPS, LV_SYMBOL_SHUFFLE, LV_SYMBOL_PLAY};
        constexpr std::array<size_t, 3> color_indices = {4, 2, 3};
        const auto game = static_cast<mini_games::GameId>(arcade_selected_);
        const size_t color_index = color_indices[arcade_selected_];

        lv_label_set_text(arcade_icon_, symbols[arcade_selected_]);
        lv_label_set_text(arcade_name_label_, mini_games::name(game));
        lv_label_set_text(arcade_description_label_, mini_games::subtitle(game));
        lv_label_set_text_fmt(arcade_index_label_, "0%d / 03", arcade_selected_ + 1);
        lv_obj_set_style_text_color(arcade_icon_, accent(color_index), 0);
        lv_obj_set_style_border_color(arcade_panel_, accent(color_index), 0);
        for (size_t index = 0; index < arcade_dots_.size(); ++index) {
            const bool selected = index == static_cast<size_t>(arcade_selected_);
            lv_obj_set_style_bg_color(arcade_dots_[index],
                                      selected ? accent(color_index)
                                               : lv_color_hex(0x46546A), 0);
            lv_obj_set_style_bg_opa(arcade_dots_[index],
                                    selected ? LV_OPA_COVER : LV_OPA_50, 0);
        }
        start_animation(arcade_panel_, anim_scale, boot ? 226 : 242, 256,
                        boot ? 520 : 320, 0, lv_anim_path_overshoot);
        start_animation(arcade_name_label_, anim_opa, LV_OPA_20, LV_OPA_COVER, 300);
    }

    size_t active_game_color() const
    {
        switch (active_game_) {
        case mini_games::GameId::tilt_quest:
            return 4;
        case mini_games::GameId::meteor_dodge:
            return 2;
        case mini_games::GameId::tap_runner:
            return 3;
        }
        return 4;
    }

    void configure_game_objects()
    {
        const bool show_tilt = active_game_ == mini_games::GameId::tilt_quest;
        const bool show_meteors = active_game_ == mini_games::GameId::meteor_dodge;
        const bool show_runner = active_game_ == mini_games::GameId::tap_runner;

        set_visible(tilt_goal_, show_tilt);
        set_visible(tilt_ball_, show_tilt);
        for (lv_obj_t* wall : tilt_walls_) {
            set_visible(wall, show_tilt);
        }
        set_visible(meteor_player_, show_meteors);
        set_visible(meteor_shield_, false);
        for (lv_obj_t* meteor : meteor_objects_) {
            set_visible(meteor, show_meteors);
        }
        set_visible(runner_ground_, show_runner);
        set_visible(runner_player_, show_runner);
        set_visible(runner_obstacle_, show_runner);

        const char* short_title = show_tilt ? "TILT" : show_meteors ? "METEOR" : "RUNNER";
        lv_label_set_text(game_title_label_, short_title);
        lv_obj_set_style_text_color(game_score_label_, accent(active_game_color()), 0);
        lv_obj_set_style_border_color(game_field_, accent(active_game_color()), 0);
        if (show_tilt) {
            lv_label_set_text(game_controls_label_, "K1 BACK   K2 CENTER");
        } else if (show_meteors) {
            lv_label_set_text(game_controls_label_, "K1 BACK   K2 SHIELD");
        } else {
            lv_label_set_text(game_controls_label_, "K1 BACK    K2 JUMP");
        }
    }

    void open_arcade_game()
    {
        active_game_ = static_cast<mini_games::GameId>(arcade_selected_);
        current_page_ = Page::game;
        configure_game_objects();
        restart_current_game();
        lv_screen_load_anim(game_screen_, LV_SCREEN_LOAD_ANIM_MOVE_LEFT,
                            kTransitionMs, 0, false);
    }

    void show_arcade()
    {
        if (mini_games::requires_motion(active_game_)) {
            request_motion_enabled(false);
        }
        game_waiting_for_motion_ = false;
        game_started_ = false;
        current_page_ = Page::arcade;
        update_arcade_selector(false);
        lv_screen_load_anim(arcade_screen_, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                            kTransitionMs, 0, false);
    }

    void show_game_overlay(const char* text)
    {
        lv_label_set_text(game_overlay_label_, text);
        set_visible(game_overlay_, true);
        lv_obj_move_foreground(game_overlay_);
        start_animation(game_overlay_, anim_opa, LV_OPA_40, LV_OPA_COVER, 260);
    }

    void hide_game_overlay()
    {
        set_visible(game_overlay_, false);
    }

    void calibrate_game_motion(const model::MotionSample& sample)
    {
        game_roll_zero_ = sample.roll_deg;
        game_pitch_zero_ = sample.pitch_deg;
    }

    static float angle_delta(float angle, float zero)
    {
        float difference = angle - zero;
        while (difference > 180.0f) {
            difference -= 360.0f;
        }
        while (difference < -180.0f) {
            difference += 360.0f;
        }
        return difference;
    }

    mini_games::RunState current_game_state() const
    {
        switch (active_game_) {
        case mini_games::GameId::tilt_quest:
            return tilt_quest_.state();
        case mini_games::GameId::meteor_dodge:
            return meteor_dodge_.state();
        case mini_games::GameId::tap_runner:
            return tap_runner_.state();
        }
        return mini_games::RunState::game_over;
    }

    void reset_game_engine()
    {
        game_motion_error_shown_ = false;
        const uint32_t seed = esp_random();
        switch (active_game_) {
        case mini_games::GameId::tilt_quest:
            tilt_quest_.reset(seed);
            lv_label_set_text(game_status_label_, "TILT / K2 CENTER");
            break;
        case mini_games::GameId::meteor_dodge:
            meteor_dodge_.reset(seed);
            lv_label_set_text(game_status_label_, "SHIELD READY");
            break;
        case mini_games::GameId::tap_runner:
            tap_runner_.reset(seed);
            lv_label_set_text(game_status_label_, "TAP K2 TO JUMP");
            break;
        }
        game_last_state_ = mini_games::RunState::playing;
        game_started_ = true;
        game_waiting_for_motion_ = false;
        game_last_update_us_ = esp_timer_get_time();
        hide_game_overlay();
        update_game_objects();
    }

    void restart_current_game()
    {
        configure_game_objects();
        game_started_ = false;
        game_motion_error_shown_ = false;
        game_last_update_us_ = esp_timer_get_time();
        if (!mini_games::requires_motion(active_game_)) {
            request_motion_enabled(false);
            reset_game_engine();
            return;
        }

        request_motion_enabled(true);
        const model::MotionSample sample = model::MotionState::instance().snapshot();
        if (motion_runtime_state() == MotionRuntimeState::running && sample.valid) {
            calibrate_game_motion(sample);
            reset_game_engine();
            return;
        }

        game_waiting_for_motion_ = true;
        lv_label_set_text(game_score_label_, "--");
        lv_label_set_text(game_status_label_, "BMI270 / STARTING");
        show_game_overlay("HOLD LEVEL\nSTARTING IMU");
    }

    void advance_carousel()
    {
        selected_ = (selected_ + 1) % static_cast<int>(cards_.size());
        ESP_LOGI(kTag, "menu selection: %d/%u", selected_ + 1,
                 static_cast<unsigned>(cards_.size()));
        update_carousel(false);
    }

    void update_carousel(bool boot)
    {
        constexpr std::array<const char*, 5> descriptions = {
            "On-demand 100 Hz VQF",
            "Layered aura animation",
            "Memory and uptime",
            "G4 + G5 square wave",
            "Three pocket games",
        };
        lv_obj_move_foreground(cards_[selected_]);
        for (size_t index = 0; index < cards_.size(); ++index) {
            const int item_count = static_cast<int>(cards_.size());
            const int relative =
                (static_cast<int>(index) - selected_ + item_count) % item_count;
            lv_obj_t* card = cards_[index];
            const bool is_selected = relative == 0;
            const bool is_neighbor = relative == 1 || relative == item_count - 1;
            const int target_x = is_selected ? 31
                                             : relative == 1 ? 105
                                                             : relative == item_count - 1 ? -43 : 179;
            const int target_y = relative == 0 ? 66 : 74;
            const int target_scale = is_selected ? 256 : is_neighbor ? 214 : 190;
            const int target_opa = is_selected ? LV_OPA_COVER
                                               : is_neighbor ? LV_OPA_50 : LV_OPA_TRANSP;

            const uint32_t delay = boot ? 90 + index * 75 : 0;
            const int from_y = boot ? 148 : lv_obj_get_y(card);
            const int from_opa = boot ? 0 : lv_obj_get_style_opa(card, LV_PART_MAIN);

            start_animation(card, anim_x, lv_obj_get_x(card), target_x, 430, delay,
                            lv_anim_path_ease_in_out);
            start_animation(card, anim_y, from_y, target_y, 430, delay, lv_anim_path_ease_out);
            start_animation(card, anim_scale, lv_obj_get_style_transform_scale_x(card, LV_PART_MAIN),
                            target_scale, 470, delay, lv_anim_path_overshoot);
            start_animation(card, anim_opa, from_opa, target_opa, 340, delay,
                            lv_anim_path_ease_out);
        }
        lv_obj_set_style_border_color(selector_glow_, accent(selected_), 0);
        lv_label_set_text(menu_hint_, descriptions[selected_]);
        start_animation(menu_hint_, anim_opa, LV_OPA_30, LV_OPA_COVER, 360, boot ? 330 : 80);

        carousel_busy_ = true;
        if (carousel_timer_) {
            lv_timer_delete(carousel_timer_);
            carousel_timer_ = nullptr;
        }
        const uint32_t unlock_delay = boot ? kCarouselBootDurationMs : kCarouselDurationMs;
        carousel_timer_ =
            lv_timer_create(carousel_timer_callback, unlock_delay, this);
        if (!carousel_timer_) {
            carousel_busy_ = false;
        }
    }

    void open_selected()
    {
        lv_obj_t* target = nullptr;
        switch (selected_) {
        case 0:
            current_page_ = Page::motion;
            prepare_motion_display();
            request_motion_enabled(true);
            target = motion_screen_;
            break;
        case 1:
            current_page_ = Page::ambient;
            target = ambient_screen_;
            break;
        case 2:
            current_page_ = Page::system;
            target = system_screen_;
            update_system();
            break;
        case 3:
            current_page_ = Page::wave;
            target = wave_screen_;
            set_wave_output(true);
            break;
        default:
            current_page_ = Page::arcade;
            target = arcade_screen_;
            update_arcade_selector(false);
            break;
        }
        lv_screen_load_anim(target, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, kTransitionMs, 0, false);
    }

    void show_menu()
    {
        if (current_page_ == Page::motion) {
            request_motion_enabled(false);
        } else if (current_page_ == Page::wave) {
            set_wave_output(false);
        } else if (current_page_ == Page::game &&
                   mini_games::requires_motion(active_game_)) {
            request_motion_enabled(false);
        }
        current_page_ = Page::menu;
        lv_screen_load_anim(menu_screen_, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, kTransitionMs, 0, false);
    }

    void update_wave_display()
    {
        auto& generator = wave::Generator::instance();
        const uint32_t frequency_hz = generator.frequency_hz();
        if (frequency_hz >= 1'000'000) {
            lv_label_set_text_fmt(wave_frequency_label_, "%lu.%03lu",
                                  static_cast<unsigned long>(frequency_hz / 1'000'000),
                                  static_cast<unsigned long>((frequency_hz % 1'000'000) / 1'000));
            lv_label_set_text(wave_unit_label_, "MHz");
        } else {
            lv_label_set_text_fmt(wave_frequency_label_, "%lu",
                                  static_cast<unsigned long>(frequency_hz / 1'000));
            lv_label_set_text(wave_unit_label_, "kHz");
        }

        lv_label_set_text_fmt(wave_duty_label_, "DUTY %u%% / 2-BIT",
                              generator.duty_percent());
        wave_enabled_ = generator.enabled();
        const lv_color_t state_color =
            wave_enabled_ ? accent(3) : lv_color_hex(0x607686);
        lv_label_set_text(wave_status_label_, wave_enabled_ ? "LIVE" : "OFF");
        lv_obj_set_style_text_color(wave_status_label_, state_color, 0);
        lv_obj_set_style_bg_color(wave_status_dot_, state_color, 0);
        lv_obj_set_style_bg_opa(wave_status_dot_, wave_enabled_ ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_shadow_color(wave_status_dot_, state_color, 0);
        lv_obj_set_style_shadow_width(wave_status_dot_, wave_enabled_ ? 10 : 0, 0);
        lv_obj_set_style_shadow_opa(wave_status_dot_, wave_enabled_ ? LV_OPA_70 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(wave_scope_, wave_enabled_ ? LV_OPA_60 : LV_OPA_30, 0);
        for (lv_obj_t* segment : wave_segments_) {
            lv_obj_set_style_bg_opa(segment, wave_enabled_ ? LV_OPA_COVER : LV_OPA_30, 0);
            lv_obj_set_style_shadow_opa(segment, wave_enabled_ ? LV_OPA_40 : LV_OPA_TRANSP, 0);
        }
    }

    void show_wave_error(const char* operation, esp_err_t result)
    {
        wave_enabled_ = wave::Generator::instance().enabled();
        ESP_LOGE(kTag, "wave generator %s failed: %s", operation, esp_err_to_name(result));
        lv_label_set_text(wave_status_label_, "ERR");
        lv_obj_set_style_text_color(wave_status_label_, accent(2), 0);
        lv_obj_set_style_bg_color(wave_status_dot_, accent(2), 0);
        lv_obj_set_style_bg_opa(wave_status_dot_, LV_OPA_COVER, 0);
    }

    void set_wave_output(bool enabled)
    {
        const esp_err_t result = wave::Generator::instance().set_enabled(enabled);
        if (result != ESP_OK) {
            show_wave_error(enabled ? "enable" : "mute", result);
            return;
        }
        update_wave_display();
    }

    void advance_wave_frequency()
    {
        const size_t next_index =
            (wave_frequency_index_ + 1) % wave::kFrequencyPresetsHz.size();
        const uint32_t requested_hz = wave::kFrequencyPresetsHz[next_index];
        const esp_err_t result = wave::Generator::instance().set_frequency(requested_hz);
        if (result != ESP_OK) {
            show_wave_error("frequency update", result);
            return;
        }

        wave_frequency_index_ = next_index;
        update_wave_display();
        for (size_t index = 0; index < wave_segments_.size(); ++index) {
            start_animation(wave_segments_[index], anim_opa, LV_OPA_20, LV_OPA_COVER,
                            240, static_cast<uint32_t>(index * 16));
        }
    }

    void advance_wave_duty()
    {
        const size_t next_index =
            (wave_duty_index_ + 1) % wave::kDutyPresetsPercent.size();
        const uint8_t requested_percent = wave::kDutyPresetsPercent[next_index];
        const esp_err_t result =
            wave::Generator::instance().set_duty_percent(requested_percent);
        if (result != ESP_OK) {
            show_wave_error("duty update", result);
            return;
        }

        wave_duty_index_ = next_index;
        update_wave_display();
        start_animation(wave_duty_label_, anim_opa, LV_OPA_20, LV_OPA_COVER, 260);
        start_animation(wave_scope_, anim_border_opa, LV_OPA_20, LV_OPA_70, 300);
    }

    void prepare_motion_display()
    {
        lv_label_set_text(motion_status_label_, "BMI270 / STARTING");
        lv_obj_set_style_text_color(motion_status_label_, accent(0), 0);
        lv_label_set_text(angles_label_, "ROLL    --.-\nPITCH   --.-\nYAW     --.-");
        lv_label_set_text(raw_label_, "a  --.- --.- --.-");
        lv_label_set_text(motion_action_label_, "K1 BACK   K2 ZERO YAW");

        yaw_zero_ = 0.0f;
        smoothed_roll_ = 0.0f;
        smoothed_pitch_ = 0.0f;
        smoothed_yaw_ = 0.0f;
        displayed_roll_ = std::numeric_limits<int32_t>::min();
        displayed_pitch_ = std::numeric_limits<int32_t>::min();
        displayed_yaw_ = std::numeric_limits<int32_t>::min();
        displayed_ax_ = std::numeric_limits<int32_t>::min();
        displayed_ay_ = std::numeric_limits<int32_t>::min();
        displayed_az_ = std::numeric_limits<int32_t>::min();
        displayed_horizon_rotation_ = 0;
        displayed_pitch_offset_ = 0;
        motion_status_state_ = -1;

        lv_obj_set_style_transform_rotation(horizon_line_, 0, 0);
        lv_obj_set_y(horizon_line_, 36);
        lv_obj_set_y(attitude_sky_, 8);
        start_animation(attitude_clip_, anim_border_opa, LV_OPA_20, LV_OPA_60, 500);
    }

    void update_tilt_objects()
    {
        const mini_games::Point& ball = tilt_quest_.ball();
        const mini_games::Point& goal = tilt_quest_.goal();
        lv_obj_set_pos(tilt_ball_,
                       2 + static_cast<int>(std::lround(
                               ball.x - mini_games::TiltQuest::kBallRadius)),
                       2 + static_cast<int>(std::lround(
                               ball.y - mini_games::TiltQuest::kBallRadius)));
        lv_obj_set_pos(tilt_goal_,
                       2 + static_cast<int>(std::lround(
                               goal.x - mini_games::TiltQuest::kGoalRadius)),
                       2 + static_cast<int>(std::lround(
                               goal.y - mini_games::TiltQuest::kGoalRadius)));
        const uint32_t seconds = static_cast<uint32_t>(
            std::ceil(tilt_quest_.seconds_remaining()));
        lv_label_set_text_fmt(game_score_label_, "%lu / %lu",
                              static_cast<unsigned long>(tilt_quest_.score()),
                              static_cast<unsigned long>(mini_games::TiltQuest::kTargetScore));
        lv_label_set_text_fmt(game_status_label_, "%lus  TILT / K2 CENTER",
                              static_cast<unsigned long>(seconds));
    }

    void update_meteor_objects()
    {
        const int player_x = 2 + static_cast<int>(std::lround(
            meteor_dodge_.player_x() - mini_games::MeteorDodge::kPlayerRadius));
        const int player_y = 2 + static_cast<int>(
            mini_games::MeteorDodge::kPlayerY - mini_games::MeteorDodge::kPlayerRadius);
        lv_obj_set_pos(meteor_player_, player_x, player_y);
        lv_obj_set_pos(meteor_shield_, player_x - 5, player_y - 5);
        set_visible(meteor_shield_, meteor_dodge_.shield_active());

        for (size_t index = 0; index < meteor_objects_.size(); ++index) {
            const mini_games::MeteorDodge::Meteor& meteor =
                meteor_dodge_.meteors()[index];
            const int diameter = std::max(
                7, static_cast<int>(std::lround(meteor.radius * 2.0f)));
            lv_obj_set_size(meteor_objects_[index], diameter, diameter);
            lv_obj_set_pos(
                meteor_objects_[index],
                2 + static_cast<int>(std::lround(meteor.center.x - meteor.radius)),
                2 + static_cast<int>(std::lround(meteor.center.y - meteor.radius)));
        }

        lv_label_set_text_fmt(game_score_label_, "SCORE %lu",
                              static_cast<unsigned long>(meteor_dodge_.score()));
        if (meteor_dodge_.shield_active()) {
            lv_label_set_text(game_status_label_, "SHIELD ACTIVE");
        } else if (meteor_dodge_.shield_ready()) {
            lv_label_set_text(game_status_label_, "SHIELD READY / K2");
        } else {
            const uint32_t tenths = static_cast<uint32_t>(
                std::ceil(meteor_dodge_.shield_cooldown_seconds() * 10.0f));
            lv_label_set_text_fmt(game_status_label_, "SHIELD %lu.%lus",
                                  static_cast<unsigned long>(tenths / 10U),
                                  static_cast<unsigned long>(tenths % 10U));
        }
    }

    void update_runner_objects()
    {
        lv_obj_set_pos(runner_player_,
                       2 + static_cast<int>(mini_games::TapRunner::kPlayerX),
                       2 + static_cast<int>(std::lround(tap_runner_.player_y())));
        const mini_games::Rect& obstacle = tap_runner_.obstacle();
        lv_obj_set_pos(runner_obstacle_,
                       2 + static_cast<int>(std::lround(obstacle.x)),
                       2 + static_cast<int>(std::lround(obstacle.y)));
        lv_obj_set_size(runner_obstacle_,
                        std::max(1, static_cast<int>(std::lround(obstacle.width))),
                        std::max(1, static_cast<int>(std::lround(obstacle.height))));
        lv_label_set_text_fmt(game_score_label_, "SCORE %lu",
                              static_cast<unsigned long>(tap_runner_.score()));
        lv_label_set_text(game_status_label_,
                          tap_runner_.on_ground() ? "TAP K2 TO JUMP" : "AIRBORNE");
    }

    void update_game_objects()
    {
        switch (active_game_) {
        case mini_games::GameId::tilt_quest:
            update_tilt_objects();
            break;
        case mini_games::GameId::meteor_dodge:
            update_meteor_objects();
            break;
        case mini_games::GameId::tap_runner:
            update_runner_objects();
            break;
        }
        lv_obj_move_foreground(game_overlay_);
    }

    void update_game()
    {
        if (game_waiting_for_motion_) {
            const MotionRuntimeState runtime = motion_runtime_state();
            const model::MotionSample sample = model::MotionState::instance().snapshot();
            if (runtime == MotionRuntimeState::running && sample.valid) {
                calibrate_game_motion(sample);
                reset_game_engine();
            } else if (runtime == MotionRuntimeState::failed &&
                       !game_motion_error_shown_) {
                game_motion_error_shown_ = true;
                lv_label_set_text(game_status_label_, "BMI270 / ERROR");
                show_game_overlay("IMU ERROR\nHOLD K2 RETRY");
            }
            return;
        }
        if (!game_started_) {
            return;
        }

        const int64_t now = esp_timer_get_time();
        const float elapsed_seconds = game_last_update_us_ == 0
                                          ? 0.0f
                                          : static_cast<float>(now - game_last_update_us_) /
                                                1000000.0f;
        game_last_update_us_ = now;

        if (active_game_ == mini_games::GameId::tilt_quest ||
            active_game_ == mini_games::GameId::meteor_dodge) {
            const model::MotionSample sample = model::MotionState::instance().snapshot();
            if (!sample.valid) {
                return;
            }
            // The fused Euler signs run opposite to the physical screen direction.
            // Keep the roll/pitch axis assignment, but invert both control signs.
            const float tilt_x = std::clamp(
                -angle_delta(sample.roll_deg, game_roll_zero_) / 20.0f, -1.0f, 1.0f);
            if (active_game_ == mini_games::GameId::tilt_quest) {
                const float tilt_y = std::clamp(
                    angle_delta(sample.pitch_deg, game_pitch_zero_) / 20.0f,
                    -1.0f, 1.0f);
                tilt_quest_.update(elapsed_seconds, tilt_x, tilt_y);
            } else {
                meteor_dodge_.update(elapsed_seconds, tilt_x);
            }
        } else {
            tap_runner_.update(elapsed_seconds);
        }

        update_game_objects();
        const mini_games::RunState state = current_game_state();
        if (state == game_last_state_) {
            if (state == mini_games::RunState::won) {
                lv_label_set_text(game_status_label_, "QUEST COMPLETE / K2 REPLAY");
            } else if (state == mini_games::RunState::game_over) {
                lv_label_set_text(game_status_label_, "K2 REPLAY / HOLD RESET");
            }
            return;
        }
        game_last_state_ = state;
        if (state == mini_games::RunState::won) {
            lv_label_set_text(game_status_label_, "QUEST COMPLETE");
            show_game_overlay("CLEARED!\nK2 REPLAY");
        } else if (state == mini_games::RunState::game_over) {
            lv_label_set_text(game_status_label_, "HOLD K2 TO RESET");
            if (active_game_ == mini_games::GameId::tilt_quest) {
                show_game_overlay("TIME UP\nK2 REPLAY");
            } else if (active_game_ == mini_games::GameId::meteor_dodge) {
                show_game_overlay("CRASHED\nK2 REPLAY");
            } else {
                show_game_overlay("TRIPPED\nK2 REPLAY");
            }
        }
    }

    static float approach_angle(float current, float target, float gain)
    {
        float difference = target - current;
        while (difference > 180.0f) {
            difference -= 360.0f;
        }
        while (difference < -180.0f) {
            difference += 360.0f;
        }
        return current + difference * gain;
    }

    void update_motion()
    {
        const MotionRuntimeState runtime = motion_runtime_state();
        const model::MotionSample sample = model::MotionState::instance().snapshot();
        if (runtime != MotionRuntimeState::running || !sample.valid) {
            const char* status_text = "BMI270 / SLEEP";
            lv_color_t status_color = lv_color_hex(0x73859B);
            int8_t status_state = 6;

            if (runtime == MotionRuntimeState::failed) {
                status_text = "BMI270 / ERROR";
                status_color = accent(2);
                status_state = 5;
            } else if (runtime == MotionRuntimeState::starting ||
                       (runtime == MotionRuntimeState::stopped &&
                        motion_enabled_requested())) {
                status_text = "BMI270 / STARTING";
                status_color = accent(0);
                status_state = 2;
            } else if (runtime == MotionRuntimeState::stopping) {
                status_text = "BMI270 / STOPPING";
                status_color = lv_color_hex(0x73859B);
                status_state = 4;
            } else if (runtime == MotionRuntimeState::running) {
                status_text = "VQF 6D / WARMUP";
                status_color = accent(1);
                status_state = 3;
            }

            if (motion_status_state_ != status_state) {
                lv_label_set_text(motion_status_label_, status_text);
                lv_obj_set_style_text_color(motion_status_label_, status_color, 0);
                motion_status_state_ = status_state;
            }
            return;
        }

        constexpr float gain = 0.22f;
        smoothed_roll_ = approach_angle(smoothed_roll_, sample.roll_deg, gain);
        smoothed_pitch_ += (sample.pitch_deg - smoothed_pitch_) * gain;
        smoothed_yaw_ = approach_angle(smoothed_yaw_, sample.yaw_deg - yaw_zero_, gain);

        const int8_t status_state = sample.rest_detected ? 1 : 0;
        if (motion_status_state_ != status_state) {
            lv_label_set_text(motion_status_label_,
                              sample.rest_detected ? "VQF 6D / REST" : "VQF 6D / LIVE");
            lv_obj_set_style_text_color(motion_status_label_,
                                        sample.rest_detected ? accent(1) : accent(0), 0);
            motion_status_state_ = status_state;
        }

        const Fixed1 roll = to_fixed_1(smoothed_roll_);
        const Fixed1 pitch = to_fixed_1(smoothed_pitch_);
        const Fixed1 yaw = to_fixed_1(smoothed_yaw_);
        if (roll.scaled != displayed_roll_ || pitch.scaled != displayed_pitch_ ||
            yaw.scaled != displayed_yaw_) {
            char angles[56];
            lv_snprintf(angles, sizeof(angles),
                        "ROLL   %c%u.%u\nPITCH  %c%u.%u\nYAW    %c%u.%u",
                        roll.sign, roll.whole, roll.fraction,
                        pitch.sign, pitch.whole, pitch.fraction,
                        yaw.sign, yaw.whole, yaw.fraction);
            lv_label_set_text(angles_label_, angles);
            displayed_roll_ = roll.scaled;
            displayed_pitch_ = pitch.scaled;
            displayed_yaw_ = yaw.scaled;
        }

        const Fixed1 ax = to_fixed_1(sample.accel_g[0]);
        const Fixed1 ay = to_fixed_1(sample.accel_g[1]);
        const Fixed1 az = to_fixed_1(sample.accel_g[2]);
        if (ax.scaled != displayed_ax_ || ay.scaled != displayed_ay_ ||
            az.scaled != displayed_az_) {
            char acceleration[32];
            lv_snprintf(acceleration, sizeof(acceleration),
                        "a %c%u.%u %c%u.%u %c%u.%u",
                        ax.sign, ax.whole, ax.fraction,
                        ay.sign, ay.whole, ay.fraction,
                        az.sign, az.whole, az.fraction);
            lv_label_set_text(raw_label_, acceleration);
            displayed_ax_ = ax.scaled;
            displayed_ay_ = ay.scaled;
            displayed_az_ = az.scaled;
        }

        const int32_t horizon_rotation = static_cast<int32_t>(-smoothed_roll_ * 10.0f);
        if (horizon_rotation != displayed_horizon_rotation_) {
            lv_obj_set_style_transform_rotation(horizon_line_, horizon_rotation, 0);
            displayed_horizon_rotation_ = horizon_rotation;
        }

        const int pitch_offset =
            std::clamp(static_cast<int>(smoothed_pitch_ * 0.42f), -22, 22);
        if (pitch_offset != displayed_pitch_offset_) {
            lv_obj_set_y(horizon_line_, 36 + pitch_offset);
            lv_obj_set_y(attitude_sky_, 8 + pitch_offset);
            displayed_pitch_offset_ = pitch_offset;
        }
    }

    void update_system()
    {
        const uint32_t heap_kb = esp_get_free_heap_size() / 1024;
        const uint32_t psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        const uint64_t uptime_seconds = esp_timer_get_time() / 1000000ULL;
        lv_label_set_text_fmt(heap_label_, "HEAP  %lu KB", heap_kb);
        lv_label_set_text_fmt(psram_label_, "PSRAM %lu KB", psram_kb);
        lv_label_set_text_fmt(uptime_label_, "UP    %llu s", uptime_seconds);
    }

    std::array<lv_obj_t*, 7> screens_{};
    lv_obj_t* menu_screen_ = nullptr;
    lv_obj_t* motion_screen_ = nullptr;
    lv_obj_t* ambient_screen_ = nullptr;
    lv_obj_t* system_screen_ = nullptr;
    lv_obj_t* wave_screen_ = nullptr;
    lv_obj_t* arcade_screen_ = nullptr;
    lv_obj_t* game_screen_ = nullptr;
    std::array<lv_obj_t*, 5> cards_{};
    lv_obj_t* selector_glow_ = nullptr;
    lv_obj_t* menu_hint_ = nullptr;

    lv_obj_t* motion_status_label_ = nullptr;
    lv_obj_t* attitude_clip_ = nullptr;
    lv_obj_t* attitude_sky_ = nullptr;
    lv_obj_t* horizon_line_ = nullptr;
    lv_obj_t* angles_label_ = nullptr;
    lv_obj_t* raw_label_ = nullptr;
    lv_obj_t* motion_action_label_ = nullptr;

    lv_obj_t* wave_scope_ = nullptr;
    lv_obj_t* wave_status_dot_ = nullptr;
    lv_obj_t* wave_status_label_ = nullptr;
    lv_obj_t* wave_frequency_label_ = nullptr;
    lv_obj_t* wave_unit_label_ = nullptr;
    lv_obj_t* wave_duty_label_ = nullptr;
    std::array<lv_obj_t*, 9> wave_segments_{};
    size_t wave_frequency_index_ = 3;
    size_t wave_duty_index_ = 1;
    bool wave_enabled_ = false;

    lv_obj_t* ambient_orb_ = nullptr;
    lv_obj_t* ambient_arc_a_ = nullptr;
    std::array<lv_obj_t*, 6> ambient_particles_{};
    size_t ambient_palette_ = 1;

    lv_obj_t* heap_label_ = nullptr;
    lv_obj_t* psram_label_ = nullptr;
    lv_obj_t* uptime_label_ = nullptr;

    lv_obj_t* arcade_panel_ = nullptr;
    lv_obj_t* arcade_index_label_ = nullptr;
    lv_obj_t* arcade_icon_ = nullptr;
    lv_obj_t* arcade_name_label_ = nullptr;
    lv_obj_t* arcade_description_label_ = nullptr;
    std::array<lv_obj_t*, 3> arcade_dots_{};
    int arcade_selected_ = 0;

    lv_obj_t* game_title_label_ = nullptr;
    lv_obj_t* game_score_label_ = nullptr;
    lv_obj_t* game_status_label_ = nullptr;
    lv_obj_t* game_controls_label_ = nullptr;
    lv_obj_t* game_field_ = nullptr;
    lv_obj_t* game_overlay_ = nullptr;
    lv_obj_t* game_overlay_label_ = nullptr;
    lv_obj_t* tilt_ball_ = nullptr;
    lv_obj_t* tilt_goal_ = nullptr;
    std::array<lv_obj_t*, mini_games::TiltQuest::kWallCount> tilt_walls_{};
    lv_obj_t* meteor_player_ = nullptr;
    lv_obj_t* meteor_shield_ = nullptr;
    std::array<lv_obj_t*, mini_games::MeteorDodge::kMeteorCount> meteor_objects_{};
    lv_obj_t* runner_ground_ = nullptr;
    lv_obj_t* runner_player_ = nullptr;
    lv_obj_t* runner_obstacle_ = nullptr;

    mini_games::TiltQuest tilt_quest_{};
    mini_games::MeteorDodge meteor_dodge_{};
    mini_games::TapRunner tap_runner_{};
    mini_games::GameId active_game_ = mini_games::GameId::tilt_quest;
    mini_games::RunState game_last_state_ = mini_games::RunState::playing;
    int64_t game_last_update_us_ = 0;
    float game_roll_zero_ = 0.0f;
    float game_pitch_zero_ = 0.0f;
    bool game_waiting_for_motion_ = false;
    bool game_motion_error_shown_ = false;
    bool game_started_ = false;

    lv_timer_t* update_timer_ = nullptr;
    lv_timer_t* carousel_timer_ = nullptr;
#ifdef M5_STICKS3_HW_SMOKE_TEST
    lv_timer_t* smoke_timer_ = nullptr;
    uint8_t smoke_step_ = 0;
#endif
    Page current_page_ = Page::menu;
    int selected_ = 0;
    bool carousel_busy_ = false;
    bool carousel_step_queued_ = false;
    float smoothed_roll_ = 0.0f;
    float smoothed_pitch_ = 0.0f;
    float smoothed_yaw_ = 0.0f;
    float yaw_zero_ = 0.0f;
    int32_t displayed_roll_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_pitch_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_yaw_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_ax_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_ay_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_az_ = std::numeric_limits<int32_t>::min();
    int32_t displayed_horizon_rotation_ = std::numeric_limits<int32_t>::min();
    int displayed_pitch_offset_ = std::numeric_limits<int>::min();
    int8_t motion_status_state_ = -1;
};

class UiModule final : public AppModule {
public:
    explicit UiModule(app::ButtonEventBus& event_bus) : event_bus_(event_bus) {}

    std::string_view name() const override { return "ui"; }

private:
    static void button_event_callback(void* context, const app::ButtonEvent& event)
    {
        auto* self = static_cast<UiModule*>(context);
        if (!self->controller_) {
            return;
        }

        if (event.id == app::ButtonId::key2 &&
            event.action == app::ButtonAction::pressed) {
            self->key2_pressed_at_ = event.timestamp;
            self->key2_pressed_ = true;
            if (!lvgl_port_lock(100)) {
                ESP_LOGW(kTag, "LVGL lock timeout while handling KEY2 press");
                return;
            }
            self->controller_->key2_pressed();
            lvgl_port_unlock();
            return;
        }
        if (event.action != app::ButtonAction::released) {
            return;
        }

        bool key2_long_press = false;
        if (event.id == app::ButtonId::key2) {
            key2_long_press =
                self->key2_pressed_ &&
                event.timestamp - self->key2_pressed_at_ >= kLongPressTicks;
            self->key2_pressed_ = false;
        }
        if (!lvgl_port_lock(100)) {
            ESP_LOGW(kTag, "LVGL lock timeout while handling button event");
            return;
        }

        if (event.id == app::ButtonId::key1) {
            self->controller_->key1();
        } else if (event.id == app::ButtonId::key2) {
            self->controller_->key2(key2_long_press);
        }
        lvgl_port_unlock();
    }

    bool on_initialize() override
    {
        const esp_err_t result = bsp::Display::instance().initialize();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "display initialization failed: %s", esp_err_to_name(result));
            return false;
        }

        controller_ = std::make_unique<UiController>();
        if (!lvgl_port_lock(0)) {
            ESP_LOGE(kTag, "failed to acquire LVGL lock while creating UI");
            return false;
        }
        const bool created = controller_->create();
        lvgl_port_unlock();
        if (!created) {
            ESP_LOGE(kTag, "failed to create UI objects");
            return false;
        }

        button_subscription_ = event_bus_.subscribe(button_event_callback, this);
        if (!button_subscription_.valid()) {
            ESP_LOGE(kTag, "button event subscriber capacity exhausted");
            return false;
        }

        bsp::Display::instance().fade_backlight(88, 650);
        ESP_LOGI(kTag, "animated LVGL menu ready; KEY1=navigate/back, KEY2=open/action");
        return true;
    }

    bool on_deinitialize() override
    {
        if (button_subscription_.valid()) {
            if (!event_bus_.unsubscribe(button_subscription_)) {
                ESP_LOGE(kTag, "failed to unsubscribe from button events");
                return false;
            }
            button_subscription_ = {};
        }

        if (controller_) {
            if (!lvgl_port_lock(100)) {
                ESP_LOGE(kTag, "failed to acquire LVGL lock while destroying UI");
                return false;
            }
            controller_->destroy();
            lvgl_port_unlock();
            controller_.reset();
        }

        return bsp::Display::instance().deinitialize() == ESP_OK;
    }

    app::ButtonEventBus& event_bus_;
    app::ButtonEventBus::Subscription button_subscription_{};
    std::unique_ptr<UiController> controller_;
    TickType_t key2_pressed_at_ = 0;
    bool key2_pressed_ = false;
};

} // namespace

std::unique_ptr<AppModule> create_ui_module(app::ButtonEventBus& event_bus)
{
    return std::make_unique<UiModule>(event_bus);
}

} // namespace app_modules
