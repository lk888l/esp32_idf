#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include "audio_service.hpp"
#include "infrared_service.hpp"
#include "peripheral_ui.hpp"

namespace fake {
audio::Snapshot audio{};
infrared::Snapshot ir{};
unsigned audio_stops = 0, ir_stops = 0, recordings = 0, tones = 0, ir_sends = 0;
esp_err_t result = ESP_OK;
}
namespace audio {
Service& Service::instance() { static Service service; return service; }
Snapshot Service::snapshot() const { return fake::audio; }
void Service::stop() { ++fake::audio_stops; fake::audio.state = State::Idle; }
esp_err_t Service::play_tone(uint32_t, uint32_t duration) {
    ++fake::tones; if (fake::result == ESP_OK) { fake::audio.state = State::Tone; fake::audio.duration_ms = duration; } return fake::result;
}
esp_err_t Service::start_meter() { fake::audio.state = State::Meter; return fake::result; }
esp_err_t Service::start_recording(uint32_t duration) {
    ++fake::recordings; fake::audio.state = State::Recording; fake::audio.duration_ms = duration; return fake::result;
}
esp_err_t Service::play_recording() { fake::audio.state = State::Playback; return fake::result; }
esp_err_t Service::clear_recording() { fake::audio.recorded_ms = 0; fake::audio.recording_available = false; return fake::result; }
esp_err_t Service::set_volume(uint8_t value) { fake::audio.volume_percent = fake::audio.effective_volume_percent = value; return fake::result; }
esp_err_t Service::set_microphone_gain(float value) { fake::audio.microphone_gain_db = value; return fake::result; }
esp_err_t Service::set_format(uint32_t rate, uint8_t bits) {
    fake::audio.sample_rate_hz = rate; fake::audio.bits_per_sample = bits; fake::audio.recorded_ms = 0; fake::audio.recording_available = false; return fake::result;
}
}
namespace infrared {
Service& Service::instance() { static Service service; return service; }
Snapshot Service::snapshot() const { return fake::ir; }
esp_err_t Service::stop() { ++fake::ir_stops; fake::ir.listening = fake::ir.learning = fake::ir.busy = false; fake::ir.state = State::Idle; return ESP_OK; }
esp_err_t Service::listen() { fake::ir.listening = true; fake::ir.state = State::Listening; return fake::result; }
esp_err_t Service::learn(uint8_t slot) { fake::ir.learning = true; fake::ir.learning_slot = slot; fake::ir.state = State::Learning; return fake::result; }
esp_err_t Service::transmit_nec(uint16_t, uint8_t, bool, uint8_t) { ++fake::ir_sends; return fake::result; }
esp_err_t Service::replay_last() { ++fake::ir_sends; return fake::result; }
esp_err_t Service::replay_slot(uint8_t) { ++fake::ir_sends; return fake::result; }
esp_err_t Service::save_last(uint8_t slot) { fake::ir.slot_used[slot] = true; return fake::result; }
esp_err_t Service::erase_slot(uint8_t slot) { fake::ir.slot_used[slot] = false; return fake::result; }
esp_err_t Service::set_carrier(uint32_t hz, uint8_t duty) { fake::ir.carrier_hz = hz; fake::ir.duty_percent = duty; return fake::result; }
}
namespace {
using Ui = app_modules::PeripheralUi;
lv_obj_t* home;
std::string active_text(lv_obj_t* parent, const char* needle)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) {
        const char* value = lv_label_get_text(parent);
        if (std::strstr(value, needle)) return value;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        auto value = active_text(lv_obj_get_child(parent, i), needle);
        if (!value.empty()) return value;
    }
    return {};
}
void refresh(Ui& ui) { lv_tick_inc(350); ui.update(); lv_obj_update_layout(lv_screen_active()); }
void focus(Ui& ui, const char* target)
{
    const std::string needle = std::string("> ") + target;
    for (unsigned i = 0; i < 16; ++i) {
        if (!active_text(lv_screen_active(), needle.c_str()).empty()) return;
        ui.next();
    }
    std::fprintf(stderr, "Missing action: %s\n", target); std::abort();
}
void click(Ui& ui) { ui.press(); assert(!ui.select(false)); refresh(ui); }
void action(Ui& ui, const char* target) { focus(ui, target); click(ui); }
void open(Ui& ui, Ui::Kind kind) { auto* screen = ui.open(kind); assert(screen); lv_screen_load(screen); refresh(ui); }
void close(Ui& ui) { lv_screen_load(home); ui.close(); }
unsigned layout_errors = 0;
void check_layout(lv_obj_t* object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class) && lv_label_get_long_mode(object) == LV_LABEL_LONG_CLIP &&
        !(lv_obj_get_y(object) >= 136 && lv_obj_get_y(object) <= 184)) {
        lv_point_t measured{};
        lv_text_get_size(&measured, lv_label_get_text(object), lv_obj_get_style_text_font(object, LV_PART_MAIN),
            lv_obj_get_style_text_letter_space(object, LV_PART_MAIN), lv_obj_get_style_text_line_space(object, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (measured.x > lv_obj_get_content_width(object) || measured.y > lv_obj_get_content_height(object)) {
            std::fprintf(stderr, "CLIPPED: [%s] needs %d x %d, has %d x %d\n", lv_label_get_text(object),
                measured.x, measured.y, lv_obj_get_content_width(object), lv_obj_get_content_height(object));
            ++layout_errors;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) check_layout(lv_obj_get_child(object, i));
}
void capture(const char* name)
{
    lv_obj_update_layout(lv_screen_active());
    check_layout(lv_screen_active());
    auto* image = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    assert(image);
    std::filesystem::create_directories("previews");
    const auto filename = std::string("previews/") + name + ".ppm";
    auto* file = std::fopen(filename.c_str(), "wb"); assert(file);
    std::fprintf(file, "P6\n%u %u\n255\n", image->header.w, image->header.h);
    for (uint32_t y = 0; y < image->header.h; ++y) {
        for (uint32_t x = 0; x < image->header.w; ++x) {
            const uint8_t* pixel = image->data + y * image->header.stride + x * 3;
            const uint8_t rgb[]{pixel[2], pixel[1], pixel[0]};
            std::fwrite(rgb, 1, sizeof(rgb), file);
        }
    }
    std::fclose(file); lv_draw_buf_destroy(image);
}
void diagnostics(Ui& ui, const char* prefix)
{
    action(ui, "Diagnostics");
    for (unsigned i = 0; i < 3; ++i) {
        capture((std::string(prefix) + std::to_string(i)).c_str()); action(ui, "More details");
    }
    assert(!ui.select(true)); refresh(ui);
}
}
int main()
{
    lv_init();
    auto* display = lv_display_create(135, 240); assert(display);
    std::array<uint8_t, 135 * 40 * 2> buffer{};
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    home = lv_obj_create(nullptr); lv_screen_load(home);
    fake::audio.initialized = true; fake::audio.state = audio::State::Idle;
    fake::ir.initialized = true; fake::ir.dma_tx = fake::ir.dma_rx = true;
    Ui ui;
    open(ui, Ui::Kind::microphone);
    capture("mic_idle");
    action(ui, "Record clip"); // Empty first recording must be available.
    assert(fake::recordings == 1); assert(fake::audio.state == audio::State::Recording);
    fake::audio.recorded_ms = 2150; fake::audio.position_ms = 2150; fake::audio.recording_available = true;
    fake::audio.rms_dbfs = -16.5f; fake::audio.peak_dbfs = -0.2f;
    refresh(ui); capture("mic_recording");
    const unsigned before_stop = fake::audio_stops;
    close(ui); assert(fake::audio_stops == before_stop + 1); assert(fake::audio.recorded_ms == 2150);
    open(ui, Ui::Kind::microphone);
    action(ui, "Gain"); capture("mic_gain"); action(ui, "Increase"); action(ui, "Apply");
    assert(fake::audio.microphone_gain_db == 30.0f);
    action(ui, "Audio format"); action(ui, "Rate"); action(ui, "Depth"); capture("audio_format");
    action(ui, "Apply format"); capture("confirm_clip");
    action(ui, "Confirm"); assert(fake::audio.sample_rate_hz == 24000 && fake::audio.bits_per_sample == 24 && !fake::audio.recorded_ms);
    diagnostics(ui, "audio_diag"); close(ui);
    open(ui, Ui::Kind::speaker); action(ui, "Play tone"); capture("speaker_playing");
    focus(ui, "Play tone"); click(ui); assert(fake::tones == 1); // Busy start is disabled.
    action(ui, "Stop audio"); action(ui, "Freq"); capture("tone_frequency");
    action(ui, "Increase"); action(ui, "Apply"); capture("speaker_ready");
    action(ui, "Tone length"); capture("tone_length"); assert(!ui.select(true)); refresh(ui);
    action(ui, "Volume"); capture("volume"); assert(!ui.select(true)); refresh(ui);
    fake::audio.state = audio::State::Unavailable; fake::audio.initialized = false; refresh(ui); capture("audio_unavailable");
    fake::audio.state = audio::State::Fault; fake::audio.initialized = true; fake::audio.last_error = ESP_ERR_INVALID_STATE; refresh(ui); capture("audio_fault");
    close(ui);
    open(ui, Ui::Kind::infrared); action(ui, "Receive");
    fake::ir.received_frames = 1; fake::ir.last_symbol_count = 34;
    fake::ir.last_decoded = {.valid=true, .repeat=false, .extended=true, .address=0x12EF, .command=0xAB, .raw=0};
    refresh(ui); capture("ir_receive");
    focus(ui, "Stop RX"); ui.press(); ++fake::ir.received_frames;
    assert(!ui.select(false)); assert(fake::ir_stops > 0); refresh(ui); // Stop survives target changes.
    action(ui, "Saved slots"); capture("ir_slots"); action(ui, "Slot 1");
    action(ui, "Save last RX"); assert(fake::ir.slot_used[0]); capture("ir_slot_saved");
    action(ui, "Learn to slot"); capture("confirm_replace"); action(ui, "Cancel");
    action(ui, "Replay slot"); assert(fake::ir_sends == 1);
    action(ui, "Delete slot"); action(ui, "Cancel"); assert(fake::ir.slot_used[0]);
    action(ui, "Delete slot"); action(ui, "Confirm"); assert(!fake::ir.slot_used[0]);
    action(ui, "Learn to slot"); capture("ir_learning");
    close(ui); assert(!fake::ir.learning);
    open(ui, Ui::Kind::infrared); action(ui, "NEC sender"); capture("ir_sender");
    action(ui, "8-bit address"); action(ui, "Address"); capture("ir_address");
    action(ui, "Increase"); action(ui, "Apply");
    action(ui, "Command"); capture("ir_command"); assert(!ui.select(true)); refresh(ui);
    action(ui, "Repeats"); capture("ir_repeats"); assert(!ui.select(true)); refresh(ui);
    action(ui, "Carrier"); capture("ir_carrier"); assert(!ui.select(true)); refresh(ui);
    action(ui, "Duty"); capture("ir_duty"); assert(!ui.select(true)); refresh(ui);
    action(ui, "Send NEC"); assert(fake::ir_sends == 2);
    assert(!ui.select(true)); refresh(ui); diagnostics(ui, "ir_diag");
    assert(ui.select(true)); close(ui);
    fake::audio.initialized = true; fake::audio.state = audio::State::Idle;
    fake::audio.volume_percent = fake::audio.effective_volume_percent = 100;
    fake::audio.clipped_samples = fake::audio.rx_overruns = fake::audio.tx_underruns = UINT32_MAX;
    open(ui, Ui::Kind::speaker); diagnostics(ui, "audio_max_diag"); close(ui);
    fake::ir.dma_tx = fake::ir.dma_rx = false;
    fake::ir.received_frames = fake::ir.transmitted_frames = fake::ir.dropped_frames = UINT32_MAX;
    open(ui, Ui::Kind::infrared); capture("ir_max_counters"); diagnostics(ui, "ir_max_diag"); close(ui);
    assert(layout_errors == 0);
    lv_display_delete(display); lv_deinit();
    std::puts("PASS: real LVGL layout, empty recording, editor/format, busy gating, slot confirmations, stop/close");
}
