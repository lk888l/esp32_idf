#include "pc_monitor_ui.hpp"
#include "pc_monitor_service.hpp"
#include "connectivity_runtime.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fake { pc_monitor::Snapshot pc; connectivity::Snapshot radio; }
namespace pc_monitor { Snapshot snapshot() { return fake::pc; } }
namespace connectivity { Snapshot snapshot() { return fake::radio; } }

namespace {
bool find_text(lv_obj_t* object, const char* text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !std::strcmp(lv_label_get_text(object), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (find_text(lv_obj_get_child(object, i), text)) return true;
    return false;
}
void bounds(lv_obj_t* object)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        auto* child = lv_obj_get_child(object, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t area{}; lv_obj_get_coords(child, &area);
        if (!(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 135 && area.y2 < 240)) {
            std::fprintf(stderr, "PC bounds: %d,%d..%d,%d text=%s\n", area.x1, area.y1, area.x2, area.y2,
                lv_obj_check_type(child, &lv_label_class) ? lv_label_get_text(child) : "<object>");
            assert(false);
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_point_t size{};
            lv_text_get_size(&size, lv_label_get_text(child), lv_obj_get_style_text_font(child, LV_PART_MAIN),
                0, 0, 1000, LV_TEXT_FLAG_NONE);
            // Host and GPU names deliberately ellipsize at one line.
            const bool name = area.y1 == 46;
            if (!name && size.x > lv_obj_get_width(child)) {
                std::fprintf(stderr, "Clipped PC label: %s (%d > %d)\n", lv_label_get_text(child),
                             int(size.x), int(lv_obj_get_width(child)));
                assert(false);
            }
            assert(lv_obj_get_height(child) <= 20);
        }
        bounds(child);
    }
}
void capture(const char* name)
{
    auto* screen = lv_screen_active(); lv_obj_update_layout(screen); bounds(screen);
    auto* image = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888); assert(image);
    std::filesystem::create_directories("previews");
    const auto path = std::string("previews/") + name + ".ppm";
    auto* output = std::fopen(path.c_str(), "wb"); assert(output);
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
    app_modules::PcMonitorUi ui;
    for (unsigned pass = 0; pass < 4; ++pass) {
        fake::pc = {}; fake::radio = {};
        auto* screen = ui.open(); assert(ui.open() == screen); lv_screen_load(screen);
        assert(find_text(screen, "WIFI OFF")); capture("pc_wifi_off");
        fake::radio.wifi.ap_active = true; std::strcpy(fake::radio.wifi.ap_address, "192.168.4.1");
        fake::pc.listening = true; lv_tick_inc(250); ui.update();
        assert(find_text(screen, "WAITING FOR PC") && find_text(screen, "192.168.4.1")); capture("pc_waiting");
        auto& sample = fake::pc.stats;
        fake::pc.received = true; fake::pc.stale = false; fake::pc.age_ms = 500;
        std::strcpy(sample.host, "WORKSTATION"); std::strcpy(sample.gpu_name, "NVIDIA RTX 5070 Ti Laptop GPU");
        sample.cpu_valid = sample.gpu_valid = sample.vram_valid = sample.gpu_temp_valid = true;
        sample.cpu_pct = 37.5f; sample.gpu_pct = 81; sample.gpu_temp_c = 55;
        sample.ram_used = 8ULL << 30; sample.ram_total = 32ULL << 30;
        sample.vram_used = 4ULL << 30; sample.vram_total = 12ULL << 30;
        lv_tick_inc(250); ui.update();
        assert(find_text(screen, "37.5%") && find_text(screen, "25.0%") && find_text(screen, "81.0%")); capture("pc_live");
        assert(!ui.select(false));
        assert(find_text(screen, "8.0 / 32.0") && find_text(screen, "4.0 / 12.0") && find_text(screen, "55.0 C")); capture("pc_details");
        fake::pc.stale = true; lv_tick_inc(250); ui.update();
        assert(find_text(screen, "N/A") && !find_text(screen, "8.0 / 32.0")); capture("pc_stale");
        fake::pc.stale = false; sample.gpu_valid = sample.vram_valid = sample.gpu_temp_valid = false;
        assert(!ui.select(false)); capture("pc_no_gpu");
        assert(find_text(screen, "N/A") && find_text(screen, "37.5%"));
        std::memset(sample.host, 'M', sizeof(sample.host) - 1);
        std::memset(sample.gpu_name, 'W', sizeof(sample.gpu_name) - 1);
        sample.cpu_pct = 100; sample.ram_used = sample.ram_total = UINT64_MAX;
        sample.vram_used = sample.vram_total = (uint64_t(1023) << 20) + 900000;
        sample.gpu_temp_c = -100; sample.vram_valid = sample.gpu_temp_valid = true;
        fake::radio.wifi.state = connectivity::WifiState::connected;
        std::strcpy(fake::radio.wifi.address, "255.255.255.255");
        lv_tick_inc(250); ui.update(); capture("pc_long_names");
        assert(!ui.select(false)); capture("pc_extreme_capacity");
        assert(ui.select(true)); lv_screen_load(home); ui.close(); ui.close();
    }
    lv_obj_delete(home); lv_display_delete(display); lv_deinit();
}
