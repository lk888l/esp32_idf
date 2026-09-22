#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "debug_ui.hpp"
#include "connectivity_runtime.hpp"
namespace fake {
debug_probe::Snapshot probe;
connectivity::Snapshot radio;
unsigned starts=0, stops=0, radio_requests=0;
}
namespace debug_probe {
Snapshot snapshot() { return fake::probe; }
void select_mode(Mode mode) { fake::probe.mode=mode; fake::probe.ready=mode!=Mode::off;
    if(mode==Mode::off) ++fake::stops; else ++fake::starts; }
void set_clock_limit(uint32_t hz) { fake::probe.limit_hz=hz; }
const char* mode_name(Mode mode) { return mode==Mode::usb?"USB DAP":mode==Mode::wifi?"WIFI DAP":"BLE DAP"; }
}
namespace connectivity {
Snapshot snapshot() { return fake::radio; }
esp_err_t request_control(const ControlRequest&) { ++fake::radio_requests; return ESP_OK; }
}
void capture(const char* name) {
    lv_obj_update_layout(lv_screen_active());
    for(unsigned i=0;i<lv_obj_get_child_count(lv_screen_active());++i) {
        auto* child=lv_obj_get_child(lv_screen_active(),i);
        assert(lv_obj_get_x(child)>=0&&lv_obj_get_y(child)>=0);
        assert(lv_obj_get_x(child)+lv_obj_get_width(child)<=135);
        assert(lv_obj_get_y(child)+lv_obj_get_height(child)<=240);
        if(lv_obj_check_type(child,&lv_label_class)&&lv_label_get_long_mode(child)==LV_LABEL_LONG_CLIP) {
            lv_point_t size{};
            lv_text_get_size(&size,lv_label_get_text(child),lv_obj_get_style_text_font(child,LV_PART_MAIN),
                lv_obj_get_style_text_letter_space(child,LV_PART_MAIN),lv_obj_get_style_text_line_space(child,LV_PART_MAIN),
                1000,LV_TEXT_FLAG_NONE);
            assert(size.x<=lv_obj_get_width(child)&&size.y<=lv_obj_get_height(child));
        }
    }
    auto* image=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888); assert(image);
    std::filesystem::create_directories("previews");
    char path[100]; std::snprintf(path,sizeof(path),"previews/%s.ppm",name);
    auto* file=std::fopen(path,"wb"); assert(file);
    std::fprintf(file,"P6\n%u %u\n255\n",image->header.w,image->header.h);
    for(unsigned y=0;y<image->header.h;++y) for(unsigned x=0;x<image->header.w;++x) {
        const auto* pixel=image->data+y*image->header.stride+x*3;
        const uint8_t rgb[]{pixel[2],pixel[1],pixel[0]}; std::fwrite(rgb,1,3,file);
    }
    std::fclose(file); lv_draw_buf_destroy(image);
}
int main() {
    lv_init(); auto* display=lv_display_create(135,240);
    std::array<uint8_t,135*40*2> buffer{};
    lv_display_set_buffers(display,buffer.data(),nullptr,buffer.size(),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,[](lv_display_t* d,const lv_area_t*,uint8_t*) { lv_display_flush_ready(d); });
    auto* home=lv_obj_create(nullptr); lv_screen_load(home);
    app_modules::DebugUi ui;
    assert(fake::starts==0); // Construction/menu browsing never enables DAP.
    for(auto mode:{debug_probe::Mode::usb,debug_probe::Mode::wifi,debug_probe::Mode::ble}) {
        auto* screen=ui.open(mode); assert(screen); lv_screen_load(screen);
        fake::probe.swd=true; fake::probe.connected=true; fake::probe.packets=1234;
        fake::radio.wifi.ap_active=true; std::strcpy(fake::radio.wifi.ap_address,"192.168.4.1");
        fake::radio.ble.enabled=true; fake::radio.ble.encrypted=true; fake::radio.ble.mtu=23;
        lv_tick_inc(200); ui.update();
        capture(mode==debug_probe::Mode::usb?"dap_usb":mode==debug_probe::Mode::wifi?"dap_wifi":"dap_ble");
        assert(!ui.select(false)); // Change ceiling from 1000 to 2000 kHz.
        capture("dap_speed");
        ui.next(); assert(!ui.select(false)); capture("dap_wiring");
        ui.next(); assert(!ui.select(false)); assert(fake::probe.mode==debug_probe::Mode::off);
        assert(!ui.select(false)); assert(fake::probe.mode==mode);
        assert(ui.select(true)); // Exit is consumed by the outer menu controller.
        lv_screen_load(home); const auto before=fake::stops; ui.close(); assert(fake::stops==before+1);
    }
    assert(fake::radio_requests==0); // Shared radio state is preserved by DAP.
    lv_obj_delete(home); lv_display_delete(display); lv_deinit();
}
