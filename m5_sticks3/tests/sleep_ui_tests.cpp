#include "sleep_ui.hpp"
#include "clock_service.hpp"
#include "sleep_service.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fake {
clock_service::Snapshot clock;
std::vector<uint32_t> requests;
bool accept = true;
esp_err_t error = ESP_OK;
}
namespace clock_service {
Snapshot snapshot() { return fake::clock; }
}
namespace sleep_service {
bool request_sleep(uint32_t timer_seconds)
{
    fake::requests.push_back(timer_seconds);
    return fake::accept;
}
esp_err_t last_error() { return fake::error; }
}
namespace {
lv_obj_t* find_text(lv_obj_t* parent, const char* value)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i) {
        auto* child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_label_class) && !std::strcmp(lv_label_get_text(child), value)) return child;
        if (auto* result = find_text(child, value)) return result;
    }
    return nullptr;
}
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
                std::fprintf(stderr, "Clipped sleep label: %s (%d > %d)\n", lv_label_get_text(child), int(size.x), int(lv_obj_get_width(child)));
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
    app_modules::SleepUi ui;
    auto* screen = ui.open(); lv_screen_load(screen);
    assert(screen == ui.open());
    lv_tick_inc(10000); ui.update();
    assert(fake::requests.empty()); // Opening and waiting must never start sleep.
    assert(find_text(screen, "BUTTONS")); capture("sleep_buttons");
    assert(!ui.select(false)); assert(find_text(screen, "5 MIN")); capture("sleep_5min");
    assert(!ui.select(false)); assert(find_text(screen, "30 MIN")); capture("sleep_30min");
    assert(!ui.select(false)); assert(find_text(screen, "1 HOUR")); capture("sleep_1hour");
    assert(!ui.select(false)); assert(find_text(screen, "BUTTONS"));
    assert(fake::requests.empty());

    fake::clock.valid = true; std::strcpy(fake::clock.time, "12:34:56"); ui.update();
    assert(find_text(screen, "12:34:56")); assert(find_text(screen, "RTC KEEPS TIME"));
    assert(!ui.select(true)); assert(find_text(screen, "3")); capture("sleep_countdown");
    lv_tick_inc(1000); ui.update(); assert(find_text(screen, "2"));
    assert(!ui.select(false)); // Selection is frozen while armed.
    assert(!ui.back()); // First K1 cancels and stays on the page.
    assert(find_text(screen, "BUTTONS"));
    lv_tick_inc(10000); ui.update(); assert(fake::requests.empty());
    assert(ui.back()); // A second K1 returns to the menu.

    // A completed countdown sends exactly one request even with extra updates/keys.
    assert(!ui.select(false)); assert(!ui.select(true));
    lv_tick_inc(2999); ui.update(); assert(fake::requests.empty()); assert(find_text(screen, "1"));
    lv_tick_inc(1); ui.update(); assert(fake::requests.size() == 1); assert(fake::requests.back() == 300);
    assert(find_text(screen, "Starting...")); capture("sleep_starting");
    lv_tick_inc(10000); ui.update(); ui.select(true); ui.select(false); ui.update();
    assert(fake::requests.size() == 1); assert(!ui.back());
    lv_screen_load(home); ui.close(); ui.close();
    lv_tick_inc(10000); ui.update(); ui.select(true); assert(fake::requests.size() == 1);

    // Closing with a countdown running cancels any future request.
    lv_screen_load(ui.open()); ui.select(true); lv_tick_inc(1000); ui.update();
    lv_screen_load(home); ui.close(); lv_tick_inc(10000); ui.update();
    assert(fake::requests.size() == 1);

    // Unsigned elapsed ticks keep a countdown correct across wraparound.
    lv_tick_inc(UINT32_MAX - lv_tick_get() - 1000);
    screen = ui.open(); lv_screen_load(screen); ui.select(true);
    lv_tick_inc(2999); ui.update(); assert(fake::requests.size() == 1);
    lv_tick_inc(1); ui.update(); assert(fake::requests.size() == 2); assert(fake::requests.back() == 0);
    lv_screen_load(home); ui.close();

    // The smoke harness uses the same countdown/request path with a short timer.
    lv_screen_load(ui.open()); ui.start_for_test(5);
    lv_tick_inc(3000); ui.update(); assert(fake::requests.size() == 3); assert(fake::requests.back() == 5);
    lv_screen_load(home); ui.close();

    // Refusal leaves the page usable and never retries automatically.
    fake::accept = false; screen = ui.open(); lv_screen_load(screen); ui.select(true);
    lv_tick_inc(3000); ui.update(); assert(fake::requests.size() == 4);
    assert(find_text(screen, "SLEEP BUSY")); capture("sleep_busy");
    lv_tick_inc(10000); ui.update(); assert(fake::requests.size() == 4); assert(ui.back());
    ui.select(false); assert(find_text(screen, "5 MIN"));
    lv_screen_load(home); ui.close();

    // A failure reported by the application after accepting the request allows retry.
    fake::accept = true; screen = ui.open(); lv_screen_load(screen); ui.select(true);
    lv_tick_inc(3000); ui.update(); assert(fake::requests.size() == 5);
    fake::error = ESP_ERR_INVALID_STATE; ui.update();
    assert(find_text(screen, "RELEASE KEYS")); assert(ui.back()); capture("sleep_release_keys");
    fake::error = ESP_OK; ui.select(true); lv_tick_inc(3000); ui.update();
    assert(fake::requests.size() == 6); assert(find_text(screen, "Starting..."));
    lv_screen_load(home); ui.close();
    lv_obj_delete(home); lv_display_delete(display); lv_deinit();
}
