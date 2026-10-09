#include "clock_ui.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fake {
clock_service::Snapshot clock;
unsigned requests = 0;
}
namespace clock_service {
Snapshot snapshot() { return fake::clock; }
const char* timezone_label() { return "UTC+8"; }
esp_err_t request_sync() { ++fake::requests; return fake::clock.online ? ESP_OK : ESP_ERR_INVALID_STATE; }
}
namespace {
void check_bounds(lv_obj_t* parent)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i) {
        auto* child = lv_obj_get_child(parent, i);
        lv_area_t area{}; lv_obj_get_coords(child, &area);
        assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 135 && area.y2 < 240);
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_point_t size{};
            lv_text_get_size(&size, lv_label_get_text(child), lv_obj_get_style_text_font(child, LV_PART_MAIN),
                lv_obj_get_style_text_letter_space(child, LV_PART_MAIN), 0, 1000, LV_TEXT_FLAG_NONE);
            if (size.x > lv_obj_get_width(child)) {
                std::fprintf(stderr, "Clipped clock label: %s (%d > %d)\n", lv_label_get_text(child), int(size.x), int(lv_obj_get_width(child)));
                assert(false);
            }
        }
        check_bounds(child);
    }
}
void capture(const char* name)
{
    auto* screen = lv_screen_active(); lv_obj_update_layout(screen); check_bounds(screen);
    auto* image = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888); assert(image);
    std::filesystem::create_directories("previews");
    char path[128]{}; std::snprintf(path, sizeof(path), "previews/%s.ppm", name);
    auto* output = std::fopen(path, "wb"); assert(output);
    std::fprintf(output, "P6\n%u %u\n255\n", image->header.w, image->header.h);
    for (unsigned y = 0; y < image->header.h; ++y) for (unsigned x = 0; x < image->header.w; ++x) {
        const auto* pixel = image->data + y * image->header.stride + x * 3;
        const uint8_t rgb[]{pixel[2], pixel[1], pixel[0]}; std::fwrite(rgb, 1, 3, output);
    }
    std::fclose(output); lv_draw_buf_destroy(image);
}
} // namespace
int main()
{
    lv_init(); auto* display = lv_display_create(135, 240);
    std::array<uint8_t, 135 * 40 * 2> buffer{};
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* home = lv_obj_create(nullptr); lv_screen_load(home);
    app_modules::ClockUi ui;
    for (unsigned pass = 0; pass < 4; ++pass) {
        fake::clock = {};
        fake::clock.state = clock_service::State::waiting_wifi;
        lv_screen_load(ui.open()); capture("clock_waiting");
        assert(!ui.select(false)); capture("clock_no_wifi");
        fake::clock.online = true; fake::clock.state = clock_service::State::syncing;
        lv_tick_inc(4000); ui.update(); capture("clock_syncing");
        fake::clock.state = clock_service::State::synced; fake::clock.valid = fake::clock.synced = true;
        std::strcpy(fake::clock.time, "23:59:59"); std::strcpy(fake::clock.date, "2026/10/09");
        std::strcpy(fake::clock.weekday, "Fri"); fake::clock.sync_age_s = 59;
        lv_tick_inc(1000); ui.update(); capture("clock_synced");
        fake::clock.online = false; lv_tick_inc(1000); ui.update(); capture("clock_offline");
        fake::clock.online = true; fake::clock.state = clock_service::State::retrying;
        fake::clock.sync_age_s = 99999; lv_tick_inc(1000); ui.update(); capture("clock_retrying");
        assert(!ui.select(false)); capture("clock_requested");
        assert(ui.select(true)); lv_screen_load(home); ui.close(); ui.close();
    }
    assert(fake::requests == 8);
    lv_obj_delete(home); lv_display_delete(display); lv_deinit();
}
