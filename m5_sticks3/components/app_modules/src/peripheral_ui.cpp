#include "peripheral_ui.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "audio_service.hpp"
#include "esp_err.h"
#include "infrared_service.hpp"

namespace app_modules {
namespace {
constexpr uint32_t kText = 0xE8EEF8, kMuted = 0x95A4BD, kDim = 0x536079;
constexpr uint32_t kError = 0xFB7185, kRefreshMs = 100;
enum class View : uint8_t { home, diagnostics, editor, slots, slot, transmitter, confirm, format };
enum class Field : uint8_t { volume, gain, frequency, tone_length, clip_length, address, command, repeats, carrier, duty };
enum class Confirmation : uint8_t { clear_clip, erase_slot, save_slot, learn_slot, format };
constexpr std::array<uint32_t, 6> kSampleRates{8000, 16000, 24000, 32000, 44100, 48000};
lv_obj_t* label(lv_obj_t* parent, int x, int y, int width, int height, const lv_font_t* font, uint32_t color)
{
    auto* object = lv_label_create(parent);
    if (!object) return nullptr;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, "");
    return object;
}
void text(lv_obj_t* object, const char* value)
{
    if (std::strcmp(lv_label_get_text(object), value) != 0) lv_label_set_text(object, value);
}
const char* audio_state(audio::State state)
{
    switch (state) {
    case audio::State::Unavailable: return "UNAVAILABLE";
    case audio::State::Idle: return "READY";
    case audio::State::Tone: return "PLAYING TONE";
    case audio::State::Meter: return "MIC LIVE";
    case audio::State::Recording: return "RECORDING";
    case audio::State::Playback: return "PLAYING CLIP";
    case audio::State::Streaming: return "STREAMING";
    case audio::State::Fault: return "AUDIO ERROR";
    }
    return "UNKNOWN";
}
int db_value(float value)
{
    return std::isfinite(value) ? static_cast<int>(std::lround(std::clamp(value, -96.0f, 0.0f))) : -96;
}
} // namespace

struct PeripheralUi::Impl {
    explicit Impl(Preferences& settings) : preferences(settings) {}
    ~Impl()
    {
        if (screen) {
            if (ir()) infrared::Service::instance().stop();
            else audio::Service::instance().stop();
            lv_obj_delete(screen);
        }
    }
    Preferences& preferences;
    Kind kind = Kind::speaker;
    View view = View::home, editor_return = View::home, confirm_return = View::home;
    Field field = Field::volume;
    Confirmation confirmation = Confirmation::clear_clip;
    lv_obj_t* screen = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* counter = nullptr;
    lv_obj_t* headline = nullptr;
    lv_obj_t* subhead = nullptr;
    lv_obj_t* meter = nullptr;
    lv_obj_t* details = nullptr;
    lv_obj_t* notice = nullptr;
    lv_obj_t* footer = nullptr;
    std::array<lv_obj_t*, 3> rows{};
    audio::Snapshot audio_status{};
    infrared::Snapshot ir_status{};
    uint32_t refresh_at = 0, message_until = 0, settle_until = 0;
    uint32_t revision = 0, pressed_revision = 0;
    uint32_t editor_value = 0, editor_min = 0, editor_max = 100;
    uint8_t selected = 0, editor_step = 0, diagnostics_page = 0;
    uint8_t format_rate = 1, format_bits = 16;
    bool armed = false, message_error = false;
    char message[72]{};
    bool ir() const { return kind == Kind::infrared; }
    uint32_t color() const { return ir() ? 0xFBBF24 : kind == Kind::microphone ? 0x34D399 : 0xFB7185; }
    bool create()
    {
        screen = lv_obj_create(nullptr);
        if (!screen) return false;
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111F), 0);
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x14132B), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_set_style_border_width(screen, 0, 0);
        title = label(screen, 7, 6, 90, 16, &lv_font_montserrat_12, color());
        counter = label(screen, 98, 6, 34, 16, &lv_font_montserrat_12, kMuted);
        headline = label(screen, 7, 25, 121, 21, &lv_font_montserrat_16, kText);
        subhead = label(screen, 7, 47, 121, 16, &lv_font_montserrat_12, kMuted);
        meter = lv_bar_create(screen);
        if (!meter) return false;
        lv_obj_set_pos(meter, 8, 66);
        lv_obj_set_size(meter, 119, 5);
        lv_bar_set_range(meter, 0, 100);
        lv_obj_set_style_bg_color(meter, lv_color_hex(0x25354B), LV_PART_MAIN);
        lv_obj_set_style_bg_color(meter, lv_color_hex(color()), LV_PART_INDICATOR);
        details = label(screen, 8, 76, 119, 34, &lv_font_montserrat_12, kText);
        notice = label(screen, 8, 114, 119, 16, &lv_font_montserrat_12, color());
        if (!title || !counter || !headline || !subhead || !details || !notice) return false;
        lv_obj_set_style_text_line_space(details, 3, 0);
        lv_label_set_long_mode(notice, LV_LABEL_LONG_SCROLL_CIRCULAR);
        for (size_t i = 0; i < rows.size(); ++i) {
            rows[i] = label(screen, 7, 136 + static_cast<int>(i) * 24, 121, 23, &lv_font_montserrat_12, kMuted);
            if (!rows[i]) return false;
            lv_obj_set_style_pad_left(rows[i], 5, 0);
            lv_obj_set_style_pad_top(rows[i], 4, 0);
            lv_obj_set_style_radius(rows[i], 5, 0);
        }
        footer = label(screen, 0, 210, 135, 30, &lv_font_montserrat_12, 0x7F8EA7);
        if (!footer) return false;
        lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
        text(footer, "K1 NEXT  K2 OK\nHOLD K2 BACK");
        return true;
    }
    void notify(const char* value, bool error = false)
    {
        std::snprintf(message, sizeof(message), "%s", value);
        message_until = lv_tick_get() + 2000;
        message_error = error;
    }
    void submitted(esp_err_t result)
    {
        if (result == ESP_OK) { notify("REQUEST QUEUED"); settle_until = lv_tick_get() + 250; }
        else if (result == ESP_ERR_NO_MEM) notify("QUEUE FULL / RETRY", true);
        else if (result == ESP_ERR_INVALID_STATE) notify("BUSY / STOP FIRST", true);
        else if (result == ESP_ERR_NOT_SUPPORTED) notify("NOT AVAILABLE", true);
        else notify(esp_err_to_name(result), true);
        refresh_at = 0;
    }
    bool settling() const { return settle_until && static_cast<int32_t>(lv_tick_get() - settle_until) < 0; }
    bool audio_ready() const
    {
        return audio_status.initialized && !settling() &&
            (audio_status.state == audio::State::Idle || audio_status.state == audio::State::Fault);
    }
    bool ir_ready() const
    {
        return ir_status.initialized && !settling() && !ir_status.busy && !ir_status.listening && !ir_status.learning;
    }
    bool has_clip() const { return audio_status.recorded_ms > 0; }
    void snapshots()
    {
        if (ir()) {
            const auto next = infrared::Service::instance().snapshot();
            // Never act on a target that changed while KEY2 was held.
            if (next.state != ir_status.state || next.slot_used != ir_status.slot_used ||
                next.received_frames != ir_status.received_frames || next.initialized != ir_status.initialized) ++revision;
            ir_status = next;
        } else {
            const auto next = audio::Service::instance().snapshot();
            if (next.state != audio_status.state || next.initialized != audio_status.initialized ||
                (next.recorded_ms != 0) != has_clip()) ++revision;
            audio_status = next;
        }
    }
    void enter(View target)
    {
        view = target;
        selected = 0;
        ++revision;
        message[0] = '\0';
        message_until = 0;
        refresh_at = 0;
    }
    uint8_t count() const
    {
        switch (view) {
        case View::home: return ir() ? 8 : kind == Kind::microphone ? 10 : 9;
        case View::slots: case View::slot: case View::editor: return 5;
        case View::transmitter: return 8;
        case View::confirm: case View::diagnostics: return 2;
        case View::format: return 4;
        }
        return 1;
    }
    uint32_t step() const
    {
        if (field == Field::gain) return 6;
        constexpr std::array<uint32_t, 4> decimal{1, 10, 100, 1000}, hexadecimal{1, 16, 256, 4096};
        return (field == Field::address || field == Field::command) ? hexadecimal[editor_step] : decimal[editor_step];
    }
    bool enabled(uint8_t item) const
    {
        if (view == View::editor) return item != 2 || field != Field::gain;
        if (view == View::slots || view == View::diagnostics) return true;
        if (view == View::format) return item != 2 || audio_ready();
        if (view == View::confirm) {
            if (item == 0) return true;
            if (confirmation == Confirmation::format) return audio_ready();
            if (confirmation == Confirmation::clear_clip) return audio_ready() && has_clip();
            if (confirmation == Confirmation::save_slot) return ir_ready() && ir_status.last_symbol_count;
            return ir_ready();
        }
        if (view == View::slot) {
            switch (item) {
            case 0: case 3: return ir_ready() && ir_status.slot_used[preferences.ir_slot];
            case 1: return ir_ready();
            case 2: return ir_ready() && ir_status.last_symbol_count;
            default: return true;
            }
        }
        if (view == View::transmitter) return item != 0 || ir_ready();
        if (ir()) {
            switch (item) {
            case 0: case 2: return ir_ready();
            case 1: return ir_status.initialized;
            case 3: return ir_ready() && ir_status.last_symbol_count;
            default: return true;
            }
        }
        if (kind == Kind::speaker) {
            switch (item) {
            case 0: return audio_ready();
            case 1: case 2: return audio_status.initialized;
            case 5: return audio_ready() && has_clip();
            default: return true;
            }
        }
        switch (item) {
        case 0: return audio_ready();
        case 1: return audio_ready();
        case 2: case 4: return audio_status.initialized;
        case 3: case 6: return audio_ready() && has_clip();
        default: return true;
        }
    }
    void item_text(uint8_t item, char* out, size_t size) const
    {
        if (view == View::editor) {
            const char* names[] = {"Decrease", "Increase", "Step", "Apply", "Cancel"};
            if (item == 2) std::snprintf(out, size, "Step %lu", static_cast<unsigned long>(step()));
            else std::snprintf(out, size, "%s", names[item]);
        } else if (view == View::confirm) {
            std::snprintf(out, size, "%s", item == 0 ? "Cancel" : "Confirm");
        } else if (view == View::format) {
            switch (item) {
            case 0: std::snprintf(out, size, "Rate %lu Hz", static_cast<unsigned long>(kSampleRates[format_rate])); break;
            case 1: std::snprintf(out, size, "Depth %u bit", format_bits); break;
            case 2: std::snprintf(out, size, "Apply format"); break;
            default: std::snprintf(out, size, "Cancel"); break;
            }
        } else if (view == View::diagnostics) {
            std::snprintf(out, size, "%s", item == 0 ? "More details" : "Back");
        } else if (view == View::slots) {
            if (item == 4) std::snprintf(out, size, "Back");
            else std::snprintf(out, size, "Slot %u  %s", item + 1, ir_status.slot_used[item] ? "saved" : "empty");
        } else if (view == View::slot) {
            const char* names[] = {"Replay slot", "Learn to slot", "Save last RX", "Delete slot", "Back"};
            std::snprintf(out, size, "%s", names[item]);
        } else if (view == View::transmitter) {
            switch (item) {
            case 0: std::snprintf(out, size, "Send NEC"); break;
            case 1: std::snprintf(out, size, "Address %04X", preferences.ir_address); break;
            case 2: std::snprintf(out, size, "Command %02X", preferences.ir_command); break;
            case 3: std::snprintf(out, size, "%s address", preferences.ir_extended ? "16-bit" : "8-bit"); break;
            case 4: std::snprintf(out, size, "Repeats %u", preferences.ir_repeats); break;
            case 5: std::snprintf(out, size, "Carrier %lu Hz", static_cast<unsigned long>(ir_status.carrier_hz)); break;
            case 6: std::snprintf(out, size, "Duty %u%%", ir_status.duty_percent); break;
            default: std::snprintf(out, size, "Back"); break;
            }
        } else if (ir()) {
            const char* names[] = {"Receive", "Stop RX / TX", "Learn slot", "Replay last RX", "Saved slots", "NEC sender", "Diagnostics", "Back"};
            if (item == 2) std::snprintf(out, size, "Learn slot %u", preferences.ir_slot + 1);
            else std::snprintf(out, size, "%s", names[item]);
        } else if (kind == Kind::speaker) {
            switch (item) {
            case 0: std::snprintf(out, size, "Play tone"); break;
            case 1: std::snprintf(out, size, "Stop audio"); break;
            case 2: std::snprintf(out, size, "Volume %u%%", audio_status.volume_percent); break;
            case 3: std::snprintf(out, size, "Freq %lu Hz", static_cast<unsigned long>(preferences.tone_hz)); break;
            case 4: std::snprintf(out, size, "Tone length"); break;
            case 5: std::snprintf(out, size, "Play mic clip"); break;
            case 6: std::snprintf(out, size, "Audio format"); break;
            case 7: std::snprintf(out, size, "Diagnostics"); break;
            default: std::snprintf(out, size, "Back"); break;
            }
        } else {
            const char* names[] = {"Start meter", "Record clip", "Stop audio", "Play clip", "Mic gain", "Clip length", "Clear clip", "Audio format", "Diagnostics", "Back"};
            if (item == 4) std::snprintf(out, size, "Gain %d dB", static_cast<int>(audio_status.microphone_gain_db));
            else std::snprintf(out, size, "%s", names[item]);
        }
    }
    void edit(Field target)
    {
        editor_return = view;
        field = target;
        editor_min = 0;
        editor_step = 0;
        switch (target) {
        case Field::volume:
            editor_value = audio_status.volume_percent; editor_max = 100; editor_step = 1; break;
        case Field::gain:
            editor_value = static_cast<uint32_t>(std::max(0.0f, audio_status.microphone_gain_db)); editor_max = 42; break;
        case Field::frequency:
            editor_value = preferences.tone_hz; editor_min = 20; editor_max = 4000; editor_step = 2; break;
        case Field::tone_length:
            editor_value = preferences.tone_ms; editor_min = 100; editor_max = 30000; editor_step = 3; break;
        case Field::clip_length:
            editor_value = preferences.recording_ms; editor_min = 100; editor_max = 30000; editor_step = 3; break;
        case Field::address:
            editor_value = preferences.ir_address; editor_max = preferences.ir_extended ? 65535 : 255; break;
        case Field::command:
            editor_value = preferences.ir_command; editor_max = 255; break;
        case Field::repeats:
            editor_value = preferences.ir_repeats; editor_max = 20; break;
        case Field::carrier:
            editor_value = ir_status.carrier_hz; editor_min = 20000; editor_max = 60000; editor_step = 3; break;
        case Field::duty:
            editor_value = ir_status.duty_percent; editor_min = 1; editor_max = 50; break;
        }
        enter(View::editor);
        selected = 1;
    }
    void confirm(Confirmation target)
    {
        confirmation = target;
        confirm_return = view;
        enter(View::confirm);
    }
    void learn()
    {
        if (ir_status.slot_used[preferences.ir_slot]) confirm(Confirmation::learn_slot);
        else submitted(infrared::Service::instance().learn(preferences.ir_slot));
    }
    void apply_editor()
    {
        esp_err_t result = ESP_OK;
        bool queued = true;
        switch (field) {
        case Field::volume: result = audio::Service::instance().set_volume(static_cast<uint8_t>(editor_value)); break;
        case Field::gain: result = audio::Service::instance().set_microphone_gain(static_cast<float>(editor_value)); break;
        case Field::carrier: result = infrared::Service::instance().set_carrier(editor_value, ir_status.duty_percent); break;
        case Field::duty: result = infrared::Service::instance().set_carrier(ir_status.carrier_hz, static_cast<uint8_t>(editor_value)); break;
        case Field::frequency: preferences.tone_hz = editor_value; queued = false; break;
        case Field::tone_length: preferences.tone_ms = editor_value; queued = false; break;
        case Field::clip_length: preferences.recording_ms = editor_value; queued = false; break;
        case Field::address: preferences.ir_address = static_cast<uint16_t>(editor_value); queued = false; break;
        case Field::command: preferences.ir_command = static_cast<uint8_t>(editor_value); queued = false; break;
        case Field::repeats: preferences.ir_repeats = static_cast<uint8_t>(editor_value); queued = false; break;
        }
        if (result != ESP_OK) { submitted(result); return; }
        enter(editor_return);
        if (queued) submitted(result);
        else notify("SETTING UPDATED");
    }
    void open_format()
    {
        format_bits = audio_status.bits_per_sample;
        const auto found = std::find(kSampleRates.begin(), kSampleRates.end(), audio_status.sample_rate_hz);
        format_rate = found == kSampleRates.end() ? 1 : static_cast<uint8_t>(found - kSampleRates.begin());
        enter(View::format);
    }
    void apply_format()
    {
        const esp_err_t result = audio::Service::instance().set_format(kSampleRates[format_rate], format_bits);
        if (result == ESP_OK) enter(View::home);
        submitted(result);
    }
    bool back()
    {
        if (view == View::home) return true;
        if (view == View::editor) enter(editor_return);
        else if (view == View::confirm) enter(confirm_return);
        else if (view == View::slot) enter(View::slots);
        else enter(View::home);
        return false;
    }
    bool select(bool long_press)
    {
        if (long_press) return back();
        if (!enabled(selected)) {
            if ((!ir() && !audio_status.initialized) || (ir() && !ir_status.initialized)) notify("SERVICE UNAVAILABLE", true);
            else if (!ir() && kind == Kind::microphone && selected == 1 && view == View::home && !audio_status.recording_available)
                notify("RECORD BUFFER UNAVAILABLE", true);
            else notify("UNAVAILABLE / STOP FIRST", true);
            return false;
        }
        auto& audio_service = audio::Service::instance();
        auto& ir_service = infrared::Service::instance();
        if (view == View::editor) {
            switch (selected) {
            case 0: editor_value = editor_value - editor_min < step() ? editor_min : editor_value - step(); break;
            case 1: editor_value = std::min(editor_max, editor_value + step()); break;
            case 2:
                if (field != Field::gain) {
                    do { editor_step = (editor_step + 1) % 4; } while (step() > editor_max - editor_min);
                }
                break;
            case 3: apply_editor(); break;
            default: enter(editor_return); break;
            }
        } else if (view == View::confirm) {
            if (selected == 0) { enter(confirm_return); return false; }
            const auto pending = confirmation;
            enter(confirm_return);
            if (pending == Confirmation::clear_clip) submitted(audio_service.clear_recording());
            else if (pending == Confirmation::erase_slot) submitted(ir_service.erase_slot(preferences.ir_slot));
            else if (pending == Confirmation::save_slot) submitted(ir_service.save_last(preferences.ir_slot));
            else if (pending == Confirmation::format) apply_format();
            else submitted(ir_service.learn(preferences.ir_slot));
        } else if (view == View::format) {
            switch (selected) {
            case 0: format_rate = (format_rate + 1) % kSampleRates.size(); break;
            case 1: format_bits = format_bits == 16 ? 24 : 16; break;
            case 2:
                if (has_clip()) confirm(Confirmation::format);
                else apply_format();
                break;
            default: enter(View::home); break;
            }
        } else if (view == View::diagnostics) {
            if (selected == 0) diagnostics_page = (diagnostics_page + 1) % 3;
            else enter(View::home);
        } else if (view == View::slots) {
            if (selected == 4) enter(View::home);
            else { preferences.ir_slot = selected; enter(View::slot); }
        } else if (view == View::slot) {
            switch (selected) {
            case 0: submitted(ir_service.replay_slot(preferences.ir_slot)); break;
            case 1: learn(); break;
            case 2:
                if (ir_status.slot_used[preferences.ir_slot]) confirm(Confirmation::save_slot);
                else submitted(ir_service.save_last(preferences.ir_slot));
                break;
            case 3: confirm(Confirmation::erase_slot); break;
            default: enter(View::slots); break;
            }
        } else if (view == View::transmitter) {
            switch (selected) {
            case 0: submitted(ir_service.transmit_nec(preferences.ir_address, preferences.ir_command, preferences.ir_extended, preferences.ir_repeats)); break;
            case 1: edit(Field::address); break;
            case 2: edit(Field::command); break;
            case 3:
                preferences.ir_extended = !preferences.ir_extended;
                if (!preferences.ir_extended) preferences.ir_address &= 0xFF;
                break;
            case 4: edit(Field::repeats); break;
            case 5: edit(Field::carrier); break;
            case 6: edit(Field::duty); break;
            default: enter(View::home); break;
            }
        } else if (ir()) {
            switch (selected) {
            case 0: submitted(ir_service.listen()); break;
            case 1: submitted(ir_service.stop()); break;
            case 2: learn(); break;
            case 3: submitted(ir_service.replay_last()); break;
            case 4: enter(View::slots); break;
            case 5: enter(View::transmitter); break;
            case 6: enter(View::diagnostics); break;
            default: return true;
            }
        } else if (kind == Kind::speaker) {
            switch (selected) {
            case 0: submitted(audio_service.play_tone(preferences.tone_hz, preferences.tone_ms)); break;
            case 1: audio_service.stop(); notify("STOP REQUESTED"); break;
            case 2: edit(Field::volume); break;
            case 3: edit(Field::frequency); break;
            case 4: edit(Field::tone_length); break;
            case 5: submitted(audio_service.play_recording()); break;
            case 6: open_format(); break;
            case 7: enter(View::diagnostics); break;
            default: return true;
            }
        } else {
            switch (selected) {
            case 0: submitted(audio_service.start_meter()); break;
            case 1: submitted(audio_service.start_recording(preferences.recording_ms)); break;
            case 2: audio_service.stop(); notify("STOP REQUESTED"); break;
            case 3: submitted(audio_service.play_recording()); break;
            case 4: edit(Field::gain); break;
            case 5: edit(Field::clip_length); break;
            case 6: confirm(Confirmation::clear_clip); break;
            case 7: open_format(); break;
            case 8: enter(View::diagnostics); break;
            default: return true;
            }
        }
        refresh_at = 0;
        return false;
    }
    void render_status()
    {
        char buffer[128];
        text(title, ir() ? "INFRARED" : kind == Kind::speaker ? "SPEAKER" : "MIC INPUT");
        if (ir()) {
            text(headline, !ir_status.initialized ? "UNAVAILABLE" : ir_status.learning ? "LEARNING" :
                ir_status.listening ? "RECEIVING" : ir_status.busy ? "TRANSMITTING" :
                ir_status.last_error != ESP_OK ? "IR ERROR" : "READY");
            auto short_count = [](char* out, size_t size, uint32_t value) {
                if (value < 1000) std::snprintf(out, size, "%lu", static_cast<unsigned long>(value));
                else {
                    const uint32_t divisor = value >= 1000000000 ? 1000000000 : value >= 1000000 ? 1000000 : 1000;
                    const char suffix = value >= 1000000000 ? 'G' : value >= 1000000 ? 'M' : 'k';
                    std::snprintf(out, size, "%lu.%c%c", static_cast<unsigned long>(value / divisor),
                        static_cast<char>('0' + (value % divisor) / (divisor / 10)), suffix);
                }
            };
            char received[16], transmitted[16];
            short_count(received, sizeof(received), ir_status.received_frames);
            short_count(transmitted, sizeof(transmitted), ir_status.transmitted_frames);
            std::snprintf(buffer, sizeof(buffer), "RX %s  TX %s", received, transmitted);
            text(subhead, buffer);
            if (ir_status.last_decoded.valid) {
                std::snprintf(buffer, sizeof(buffer), "%s %04X : %02X\n%s %u syms",
                    ir_status.last_decoded.extended ? "EXT" : "NEC", ir_status.last_decoded.address, ir_status.last_decoded.command,
                    ir_status.last_decoded.repeat ? "Repeat" : "Frame", static_cast<unsigned>(ir_status.last_symbol_count));
            } else std::snprintf(buffer, sizeof(buffer), "%s\n%u raw symbols",
                    ir_status.last_symbol_count ? "RAW RECEIVED" : "No received frame", static_cast<unsigned>(ir_status.last_symbol_count));
            text(details, buffer);
            lv_bar_set_value(meter, ir_status.learning || ir_status.listening ? 100 : 0, LV_ANIM_OFF);
        } else {
            text(headline, audio_state(audio_status.state));
            if (kind == Kind::speaker) {
                std::snprintf(buffer, sizeof(buffer), "%lu Hz  VOL %u%%",
                    static_cast<unsigned long>(preferences.tone_hz), audio_status.effective_volume_percent);
                text(subhead, buffer);
                std::snprintf(buffer, sizeof(buffer), "Tone %lu.%lu s\nClip %lu.%lu s",
                    static_cast<unsigned long>(preferences.tone_ms / 1000), static_cast<unsigned long>((preferences.tone_ms % 1000) / 100),
                    static_cast<unsigned long>(audio_status.recorded_ms / 1000), static_cast<unsigned long>((audio_status.recorded_ms % 1000) / 100));
                text(details, buffer);
                const uint32_t progress = audio_status.duration_ms
                    ? static_cast<uint32_t>(std::min<uint64_t>(100, static_cast<uint64_t>(audio_status.position_ms) * 100 / audio_status.duration_ms))
                    : audio_status.effective_volume_percent;
                lv_bar_set_value(meter, progress, LV_ANIM_OFF);
            } else {
                std::snprintf(buffer, sizeof(buffer), "RMS %d  PK %d", db_value(audio_status.rms_dbfs), db_value(audio_status.peak_dbfs));
                text(subhead, buffer);
                const bool live = audio_status.state == audio::State::Meter || audio_status.state == audio::State::Recording || audio_status.state == audio::State::Streaming;
                std::snprintf(buffer, sizeof(buffer), "Gain %d dB / %s\nClip %lu.%lu/%lu s",
                    static_cast<int>(audio_status.microphone_gain_db), live ? "LIVE" : "OFF",
                    static_cast<unsigned long>(audio_status.recorded_ms / 1000), static_cast<unsigned long>((audio_status.recorded_ms % 1000) / 100),
                    static_cast<unsigned long>(preferences.recording_ms / 1000));
                text(details, buffer);
                lv_bar_set_value(meter, live ? (db_value(audio_status.rms_dbfs) + 96) * 100 / 96 : 0, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(meter, lv_color_hex(audio_status.peak_dbfs >= -1.0f ? kError : color()), LV_PART_INDICATOR);
            }
        }
    }
    void render_context()
    {
        char buffer[128];
        if (view == View::editor) {
            const char* names[] = {"VOLUME", "MIC GAIN", "FREQUENCY", "TONE LENGTH", "CLIP LENGTH",
                "NEC ADDRESS", "NEC COMMAND", "NEC REPEATS", "IR CARRIER", "CARRIER DUTY"};
            text(headline, names[static_cast<uint8_t>(field)]);
            const char* unit = field == Field::volume || field == Field::duty ? "%" : field == Field::gain ? "dB" :
                field == Field::frequency || field == Field::carrier ? "Hz" : field == Field::tone_length || field == Field::clip_length ? "ms" : "";
            if (field == Field::address || field == Field::command)
                std::snprintf(buffer, sizeof(buffer), "0x%04lX", static_cast<unsigned long>(editor_value));
            else std::snprintf(buffer, sizeof(buffer), "%lu %s", static_cast<unsigned long>(editor_value), unit);
            text(subhead, buffer);
            std::snprintf(buffer, sizeof(buffer), "%lu - %lu\nApply to keep", static_cast<unsigned long>(editor_min), static_cast<unsigned long>(editor_max));
            text(details, buffer);
            lv_bar_set_value(meter, (editor_value - editor_min) * 100 / std::max<uint32_t>(1, editor_max - editor_min), LV_ANIM_OFF);
        } else if (view == View::format) {
            text(headline, "AUDIO FORMAT");
            std::snprintf(buffer, sizeof(buffer), "%lu Hz / %u bit", static_cast<unsigned long>(kSampleRates[format_rate]), format_bits);
            text(subhead, buffer);
            text(details, "Mono PCM / DMA\nApply clears clip");
        } else if (view == View::confirm) {
            const bool clip = confirmation == Confirmation::clear_clip || confirmation == Confirmation::format;
            text(headline, clip ? "CLEAR CLIP?" : confirmation == Confirmation::erase_slot ? "DELETE SLOT?" : "REPLACE SLOT?");
            if (clip) text(subhead, "Volatile recording");
            else { std::snprintf(buffer, sizeof(buffer), "Saved slot %u", preferences.ir_slot + 1); text(subhead, buffer); }
            text(details, "Current data will\nbe replaced / lost");
        } else if (view == View::slots || view == View::slot) {
            text(headline, view == View::slots ? "SAVED SIGNALS" : "SLOT ACTIONS");
            std::snprintf(buffer, sizeof(buffer), "Selected slot %u", preferences.ir_slot + 1);
            text(subhead, buffer);
            text(details, "4 persistent slots\nNEC + raw signals");
        } else if (view == View::transmitter) {
            text(headline, "NEC SENDER");
            std::snprintf(buffer, sizeof(buffer), "ADDR %04X : %02X", preferences.ir_address, preferences.ir_command);
            text(subhead, buffer);
            std::snprintf(buffer, sizeof(buffer), "%s / %lu kHz\n%u repeat frames",
                preferences.ir_extended ? "Extended" : "Standard", static_cast<unsigned long>(ir_status.carrier_hz / 1000), preferences.ir_repeats);
            text(details, buffer);
        } else if (view == View::diagnostics) {
            text(headline, "DIAGNOSTICS");
            std::snprintf(buffer, sizeof(buffer), "PAGE %u / 3", diagnostics_page + 1);
            text(subhead, buffer);
            if (ir()) {
                if (diagnostics_page == 0) std::snprintf(buffer, sizeof(buffer), "TX DMA: %s\nRX DMA: %s",
                    ir_status.dma_tx ? "YES" : "NO", ir_status.dma_rx ? "YES" : "NO");
                else if (diagnostics_page == 1) std::snprintf(buffer, sizeof(buffer), "Drop %lu\nPulses %u",
                    static_cast<unsigned long>(ir_status.dropped_frames), static_cast<unsigned>(ir_status.last_symbol_count));
                else std::snprintf(buffer, sizeof(buffer), "Error 0x%X\nSlots used %u / 4", static_cast<unsigned>(ir_status.last_error),
                    static_cast<unsigned>(std::count(ir_status.slot_used.begin(), ir_status.slot_used.end(), true)));
            } else {
                if (diagnostics_page == 0) std::snprintf(buffer, sizeof(buffer), "%lu Hz / I2S DMA\n%u-bit mono PCM",
                    static_cast<unsigned long>(audio_status.sample_rate_hz), audio_status.bits_per_sample);
                else if (diagnostics_page == 1) std::snprintf(buffer, sizeof(buffer), "RX ovf %lu\nTX udf %lu",
                    static_cast<unsigned long>(audio_status.rx_overruns), static_cast<unsigned long>(audio_status.tx_underruns));
                else std::snprintf(buffer, sizeof(buffer), "Clip %lu\nError 0x%X",
                    static_cast<unsigned long>(audio_status.clipped_samples), static_cast<unsigned>(audio_status.last_error));
            }
            text(details, buffer);
        }
    }
    void render_notice()
    {
        const bool active_message = message[0] && static_cast<int32_t>(lv_tick_get() - message_until) < 0;
        const esp_err_t last_error = ir() ? ir_status.last_error : audio_status.last_error;
        const bool is_error = active_message ? message_error : last_error != ESP_OK;
        lv_obj_set_style_text_color(notice, lv_color_hex(is_error ? kError : color()), 0);
        if (active_message) text(notice, message);
        else if (last_error != ESP_OK) text(notice, esp_err_to_name(last_error));
        else if (settling()) text(notice, "REQUEST QUEUED");
        else if (view == View::editor) text(notice, "HOLD K2 CANCEL");
        else if (ir() && ir_status.learning) text(notice, "AIM REMOTE / PRESS");
        else if (ir() && (ir_status.listening || ir_status.busy)) text(notice, "ACTIVE / STOP FIRST");
        else if (!ir() && (audio_status.state == audio::State::Meter || audio_status.state == audio::State::Recording))
            text(notice, "MIC ON / EXIT STOPS");
        else if (!ir() && audio_status.effective_volume_percent < audio_status.volume_percent)
            text(notice, "BATTERY VOLUME LIMIT");
        else if (!enabled(selected)) text(notice, "ACTION UNAVAILABLE");
        else if (view == View::home && kind == Kind::microphone && selected == 1 && has_clip()) text(notice, "REPLACES LAST CLIP");
        else text(notice, "SELECT AN ACTION");
    }
    void render_rows()
    {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "%u/%u", selected + 1, count());
        text(counter, buffer);
        for (size_t i = 0; i < rows.size(); ++i) {
            const uint8_t item = (selected + count() + static_cast<int>(i) - 1) % count();
            const bool focused = i == 1;
            if (count() == 2 && i == 0) { lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN); continue; }
            lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            char name[64];
            item_text(item, name, sizeof(name));
            std::snprintf(buffer, sizeof(buffer), "%s%s", focused ? "> " : "  ", name);
            text(rows[i], buffer);
            lv_obj_set_style_text_color(rows[i], lv_color_hex(!enabled(item) ? kDim : focused ? kText : kMuted), 0);
            lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x25354B), 0);
            lv_obj_set_style_bg_opa(rows[i], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            const auto mode = focused ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
            if (lv_label_get_long_mode(rows[i]) != mode) lv_label_set_long_mode(rows[i], mode);
        }
    }
    void tick(bool force = false)
    {
        if (!screen) return;
        const uint32_t now = lv_tick_get();
        if (!force && refresh_at && static_cast<int32_t>(now - refresh_at) < 0) return;
        refresh_at = now + kRefreshMs;
        snapshots();
        render_status();
        render_context();
        // Fit headings to the real 135 px panel with the same fonts as firmware.
        lv_point_t heading_size{};
        const lv_font_t* heading_font = &lv_font_montserrat_16;
        for (const auto* font : {&lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12}) {
            heading_font = font;
            lv_text_get_size(&heading_size, lv_label_get_text(headline), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (heading_size.x <= 121) break;
        }
        lv_obj_set_style_text_font(headline, heading_font, 0);
        render_notice();
        render_rows();
    }
};
PeripheralUi::PeripheralUi() = default;
PeripheralUi::~PeripheralUi() = default;
lv_obj_t* PeripheralUi::open(Kind kind)
{
    if (impl_) return nullptr;
    impl_.reset(new (std::nothrow) Impl(preferences_));
    if (!impl_) return nullptr;
    impl_->kind = kind;
    if (!impl_->create()) { impl_.reset(); return nullptr; }
    impl_->tick(true);
    return impl_->screen;
}
void PeripheralUi::close() { impl_.reset(); }
void PeripheralUi::next()
{
    if (!impl_) return;
    impl_->selected = (impl_->selected + 1) % impl_->count();
    ++impl_->revision;
    impl_->tick(true);
}
void PeripheralUi::press()
{
    if (!impl_) return;
    impl_->snapshots();
    impl_->pressed_revision = impl_->revision;
    impl_->armed = true;
}
bool PeripheralUi::select(bool long_press)
{
    if (!impl_) return true;
    impl_->snapshots();
    const bool stable = impl_->armed && impl_->pressed_revision == impl_->revision;
    impl_->armed = false;
    const bool stop_action = impl_->view == View::home &&
        impl_->selected == (impl_->kind == Kind::microphone ? 2 : 1);
    if (!long_press && !stable && !stop_action) {
        impl_->notify("STATE CHANGED / RETRY", true);
        impl_->tick(true);
        return false;
    }
    const bool leave = impl_->select(long_press);
    if (!leave) impl_->tick(true);
    return leave;
}
void PeripheralUi::update() { if (impl_) impl_->tick(); }
} // namespace app_modules
