#include "pc_monitor_ui.hpp"

#include <cstdio>
#include <cstring>
#include "connectivity_runtime.hpp"
#include "pc_monitor_service.hpp"

namespace app_modules {
namespace {
constexpr uint32_t colors[]{0x67E8F9, 0xA78BFA, 0x34D399};
lv_obj_t* label(lv_obj_t* parent, const char* value, int x, int y, int width,
                const lv_font_t* font, uint32_t color)
{
    auto* object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_height(object, lv_font_get_line_height(font));
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_label_set_text(object, value);
    return object;
}
void text(lv_obj_t* object, const char* value)
{
    if (std::strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}
void capacity(char* output, size_t size, uint64_t used, uint64_t total, const char*& unit)
{
    static constexpr const char* units[]{"MiB", "GiB", "TiB", "PiB", "EiB"};
    double divisor = 1048576.0;
    unsigned index = 0;
    while (double(total) / divisor >= 1024 && index < 4) { divisor *= 1024; ++index; }
    unit = units[index];
    std::snprintf(output, size, "%.1f / %.1f", double(used) / divisor, double(total) / divisor);
}
} // namespace

lv_obj_t* PcMonitorUi::open()
{
    if (screen_) return screen_;
    details_ = false;
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_size(screen_, 135, 240);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_border_width(screen_, 0, 0);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x08121F), 0);
    lv_obj_set_style_bg_grad_color(screen_, lv_color_hex(0x152C3D), 0);
    lv_obj_set_style_bg_grad_dir(screen_, LV_GRAD_DIR_VER, 0);
    label(screen_, "PC MONITOR", 8, 7, 119, &lv_font_montserrat_14, 0xE8F5FF);
    status_ = label(screen_, "WAITING FOR PC", 8, 29, 119, &lv_font_montserrat_12, 0xFBBF24);
    host_ = label(screen_, "wl1-studio", 8, 46, 119, &lv_font_montserrat_12, 0x91A7BC);
    for (unsigned i = 0; i < 3; ++i) {
        auto* panel = lv_obj_create(screen_);
        lv_obj_set_pos(panel, 8, 65 + int(i) * 47);
        lv_obj_set_size(panel, 119, 43);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_set_style_radius(panel, 9, 0);
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x173347), 0);
        captions_[i] = label(panel, "", 6, 4, 107, &lv_font_montserrat_12, colors[i]);
        values_[i] = label(panel, "N/A", 6, 17, 107, &lv_font_montserrat_14, 0xF2FAFF);
        lv_obj_set_style_text_align(values_[i], LV_TEXT_ALIGN_RIGHT, 0);
        bars_[i] = lv_bar_create(panel);
        lv_obj_set_pos(bars_[i], 6, 35);
        lv_obj_set_size(bars_[i], 107, 4);
        lv_bar_set_range(bars_[i], 0, 1000);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(0x294959), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(colors[i]), LV_PART_INDICATOR);
    }
    address_ = label(screen_, "", 8, 207, 119, &lv_font_montserrat_12, 0x91A7BC);
    lv_obj_set_style_text_align(address_, LV_TEXT_ALIGN_CENTER, 0);
    auto* footer = label(screen_, "K1 BACK  K2 PAGE", 4, 224, 127, &lv_font_montserrat_12, 0x718BA2);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    refreshed_ = lv_tick_get() - 250;
    update();
    return screen_;
}

void PcMonitorUi::close()
{
    if (screen_) lv_obj_delete(screen_);
    screen_ = status_ = host_ = address_ = nullptr;
    captions_.fill(nullptr); values_.fill(nullptr); bars_.fill(nullptr);
}

bool PcMonitorUi::select(bool long_press)
{
    if (long_press) return true;
    details_ = !details_;
    refreshed_ = lv_tick_get() - 250;
    update();
    return false;
}

void PcMonitorUi::update()
{
    if (!screen_ || lv_tick_get() - refreshed_ < 250) return;
    refreshed_ = lv_tick_get();
    const auto data = pc_monitor::snapshot();
    const auto& sample = data.stats;
    const bool fresh = data.listening && data.received && !data.stale;
    char status[32]{};
    if (fresh) std::snprintf(status, sizeof(status), "LIVE  %.1fs", double(data.age_ms) / 1000);
    else std::snprintf(status, sizeof(status), "%s", data.error ? "UDP RETRYING" :
        !data.listening ? "WIFI OFF" : data.received ? "STALE / RETRY" : "WAITING FOR PC");
    text(status_, status);
    lv_obj_set_style_text_color(status_, lv_color_hex(fresh ? 0x34D399 : 0xFBBF24), 0);
    text(host_, details_ ? (sample.gpu_name[0] ? sample.gpu_name : "GPU unavailable") :
        (sample.host[0] ? sample.host : "wl1-studio / UDP"));
    const auto radio = connectivity::snapshot();
    text(address_, radio.wifi.state == connectivity::WifiState::connected ? radio.wifi.address :
        radio.wifi.ap_active ? radio.wifi.ap_address : "Connect in WIFI");
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_style_text_font(values_[i], details_ ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
        char value[48]{}, caption[24]{};
        double percent = 0;
        bool valid = fresh;
        if (!details_) {
            const char* names[]{"CPU", "RAM", "GPU"};
            text(captions_[i], names[i]);
            if (i == 0) { valid &= sample.cpu_valid; percent = sample.cpu_pct; }
            if (i == 1) { valid &= sample.ram_total != 0;
                if (sample.ram_total) percent = double(sample.ram_used) / double(sample.ram_total) * 100; }
            if (i == 2) { valid &= sample.gpu_valid; percent = sample.gpu_pct; }
            if (valid) std::snprintf(value, sizeof(value), "%.1f%%", percent);
        } else if (i < 2) {
            const bool ram = i == 0;
            valid &= ram || sample.vram_valid;
            const char* unit = "GiB";
            if (valid) capacity(value, sizeof(value), ram ? sample.ram_used : sample.vram_used,
                                ram ? sample.ram_total : sample.vram_total, unit);
            std::snprintf(caption, sizeof(caption), "%s (%s)", ram ? "RAM" : "VRAM", unit);
            text(captions_[i], caption);
        } else {
            text(captions_[i], "GPU TEMP");
            valid &= sample.gpu_temp_valid;
            if (valid) std::snprintf(value, sizeof(value), "%.1f C", double(sample.gpu_temp_c));
        }
        text(values_[i], valid ? value : "N/A");
        lv_bar_set_value(bars_[i], valid ? int32_t(percent * 10 + 0.5) : 0, LV_ANIM_OFF);
        if (details_) lv_obj_add_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
}
} // namespace app_modules
