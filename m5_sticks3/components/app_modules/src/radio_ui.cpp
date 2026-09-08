#include "radio_ui.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include "connectivity_runtime.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace app_modules {
namespace {
constexpr uint32_t kWifi = 0x67E8F9, kBle = 0xA78BFA, kText = 0xE8EEF8;
constexpr uint32_t kMuted = 0x95A4BD, kError = 0xFB7185;
constexpr size_t kHistoryCount = 16;
enum class View : uint8_t { overview, nearby, target, link, traffic, services,
    credentials, password_group, password_character, confirm_radio_off };
constexpr std::array<const char*, 10> kGroups = {
    "abcdefghijklm", "nopqrstuvwxyz", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ",
    "0123456789", "!\"#$%&'()*+,-./", ":;<=>?@", "[\\]^_\x60", "{|}~", " "
};
constexpr std::array<const char*, 10> kGroupNames = {
    "a-m", "n-z", "A-M", "N-Z", "0-9", "!-/",
    ":;<=>?@", "[\\]^_\x60", "{|}~", "SPACE"
};

void wipe(char* data, size_t size)
{
    volatile char* out = data;
    while (size--) *out++ = 0;
}

// Radio names and echoes are untrusted text. Keep original SSID bytes in
// value snapshots for connecting; only sanitize the displayed representation.
void printable(char* output, size_t capacity, const char* input)
{
    if (!capacity) return;
    size_t index = 0;
    while (input[index] && index + 1 < capacity) {
        const auto byte = static_cast<unsigned char>(input[index]);
        output[index] = byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '?';
        ++index;
    }
    output[index] = '\0';
}
void fit_width(char* value, int32_t width)
{
    lv_point_t measured{};
    size_t length = std::strlen(value);
    bool shortened = false;
    while (length) {
        lv_text_get_size(&measured, value, &lv_font_montserrat_12, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (measured.x <= width) break;
        value[--length] = '\0';
        shortened = true;
    }
    if (shortened && length) value[length - 1] = '~';
}
const char* tail(const char* input, size_t offset)
{
    return input + std::min(offset, std::strlen(input));
}
const char* auth_name(uint8_t auth)
{
    switch (auth) {
    case 0: return "OPEN";
    case 1: return "WEP";
    case 2: return "WPA1";
    case 3: return "WPA2";
    case 4: return "WPA/WPA2";
    case 5: return "ENTERPRISE";
    case 6: return "WPA3";
    case 7: return "WPA2/WPA3";
    case 9: return "OWE";
    case 11: case 12: return "WPA3";
    default: return "OTHER";
    }
}
const char* auth_short(uint8_t auth)
{
    if (auth == 4) return "WPA2+";
    if (auth == 5) return "EAP";
    if (auth == 7) return "WPA2/3";
    return auth_name(auth);
}
bool supported_auth(uint8_t auth)
{
    return auth == 0 || auth == 3 || auth == 4 || auth == 6 ||
           auth == 7 || auth == 11 || auth == 12;
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, int width, int height,
                const lv_font_t* font, uint32_t color)
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
void show(lv_obj_t* object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void set_text(lv_obj_t* object, const char* value)
{
    if (std::strcmp(lv_label_get_text(object), value) != 0)
        lv_label_set_text(object, value);
}
} // namespace

struct RadioUi::Impl {
    Kind kind = Kind::wifi;
    View view = View::overview;
    lv_obj_t* screen = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* headline = nullptr;
    lv_obj_t* subhead = nullptr;
    lv_obj_t* panel = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* action = nullptr;
    lv_obj_t* footer = nullptr;
    std::array<lv_obj_t*, 3> rows{};
    std::array<lv_obj_t*, kHistoryCount> bars{};
    std::array<int8_t, kHistoryCount> history{};
    size_t history_cursor = 0;
    uint32_t refresh_at = 0;
    uint32_t revision = 0, pressed_revision = 0;
    bool press_armed = false;
    uint8_t choice = 0, selected = 0, page = 0, group = 0, character = 0;
    bool waiting_for_scan = false;
    uint32_t scan_start_generation = 0;
    uint32_t scan_start_completed = 0, scan_deadline = 0;
    char message[44]{};
    uint32_t message_until = 0;
    char password[65]{};
    // Freeze each completed scan. A second value copy fixes the target through
    // later scans, so a button release cannot act on a newly reordered device.
    connectivity::WifiDiagnostics wifi_list{};
    connectivity::BleDiagnostics ble_list{};
    connectivity::WifiNetwork wifi_target{};
    connectivity::BleDevice ble_target{};
    // Reuse live POD storage on the heap. Fetching value-returning diagnostics
    // has a bounded temporary frame that must unwind before LVGL rendering.
    connectivity::Snapshot live_status{};
    connectivity::WifiDiagnostics live_wifi{};
    connectivity::BleDiagnostics live_ble{};
    connectivity::TrafficSnapshot live_traffic{};

    void refresh_snapshots()
    {
        const bool previously_enabled = wifi() ? live_wifi.enabled : live_ble.enabled;
        live_status = connectivity::snapshot();
        live_wifi = connectivity::wifi_diagnostics();
        live_ble = connectivity::ble_diagnostics();
        live_traffic = connectivity::traffic_snapshot();
        // A remote command can flip the selected ON/OFF action while KEY2 is
        // held. Treat that semantic change like a changed list/selection.
        if (view == View::overview && choice == 6 &&
            previously_enabled != (wifi() ? live_wifi.enabled : live_ble.enabled))
            ++revision;
    }

    uint32_t color() const { return kind == Kind::wifi ? kWifi : kBle; }
    bool wifi() const { return kind == Kind::wifi; }

    bool create()
    {
        history.fill(-100);
        screen = lv_obj_create(nullptr);
        if (!screen) return false;
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111F), 0);
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x14132B), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_set_style_border_width(screen, 0, 0);
        title = label(screen, 8, 7, 119, 17, &lv_font_montserrat_12, color());
        headline = label(screen, 8, 28, 119, 20, &lv_font_montserrat_18, kText);
        subhead = label(screen, 8, 49, 119, 15, &lv_font_montserrat_12, kMuted);
        panel = lv_obj_create(screen);
        if (!panel) return false;
        lv_obj_set_pos(panel, 6, 68);
        lv_obj_set_size(panel, 123, 121);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x17233D), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x395071), 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_radius(panel, 10, 0);
        lv_obj_set_style_pad_all(panel, 0, 0);
        body = label(panel, 6, 6, 109, 108, &lv_font_montserrat_12, kText);
        if (!body) return false;
        lv_obj_set_style_text_line_space(body, 3, 0);
        for (size_t i = 0; i < rows.size(); ++i) {
            rows[i] = label(panel, 4, 3 + static_cast<int>(i) * 39,
                            113, 37, &lv_font_montserrat_12, kMuted);
            if (!rows[i]) return false;
            lv_obj_set_style_pad_left(rows[i], 3, 0);
            lv_obj_set_style_pad_top(rows[i], 3, 0);
            lv_obj_set_style_radius(rows[i], 5, 0);
            lv_obj_set_style_text_line_space(rows[i], 3, 0);
        }
        for (size_t i = 0; i < bars.size(); ++i) {
            bars[i] = lv_obj_create(screen);
            if (!bars[i]) return false;
            lv_obj_remove_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(bars[i], lv_color_hex(color()), 0);
            lv_obj_set_style_border_width(bars[i], 0, 0);
            lv_obj_set_style_radius(bars[i], 1, 0);
            lv_obj_set_size(bars[i], 5, 1);
            lv_obj_set_pos(bars[i], 8 + static_cast<int>(i) * 7, 185);
        }
        action = label(screen, 7, 193, 121, 18, &lv_font_montserrat_12, color());
        footer = label(screen, 0, 212, 135, 28, &lv_font_montserrat_12, 0x7F8EA7);
        if (!title || !headline || !subhead || !action || !footer) return false;
        lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(footer, "K1 NEXT  K2 OK\nHOLD K2 BACK");
        return true;
    }
    void notify(const char* value, bool error = false)
    {
        std::snprintf(message, sizeof(message), "%s", value);
        message_until = lv_tick_get() + 2400;
        lv_obj_set_style_text_color(action, lv_color_hex(error ? kError : color()), 0);
    }
    void queue(connectivity::ControlRequest& request)
    {
        const esp_err_t result = connectivity::request_control(request);
        if (result == ESP_OK) notify("QUEUED");
        else if (result == ESP_ERR_NO_MEM) notify("QUEUE FULL / RETRY", true);
        else if (result == ESP_ERR_NOT_SUPPORTED) notify("NOT AVAILABLE", true);
        else notify(esp_err_to_name(result), true);
    }
    void command(connectivity::ControlAction request_action)
    {
        connectivity::ControlRequest request{};
        request.action = request_action;
        queue(request);
    }
    void enter(View target)
    {
        view = target;
        ++revision;
        selected = page = 0;
        message[0] = '\0';
        refresh_at = 0;
    }
    void scan()
    {
        scan_start_completed = live_status.completed;
        scan_deadline = lv_tick_get() + 20000;
        scan_start_generation = wifi() ? live_wifi.generation : live_ble.generation;
        connectivity::ControlRequest request{};
        request.action = wifi() ? connectivity::ControlAction::wifi_scan
                                : connectivity::ControlAction::ble_scan;
        const esp_err_t result = connectivity::request_control(request);
        enter(View::nearby);
        waiting_for_scan = result == ESP_OK;
        if (result != ESP_OK) notify(esp_err_to_name(result), true);
    }
    void copy_results()
    {
        if (wifi()) wifi_list = live_wifi;
        else ble_list = live_ble;
        waiting_for_scan = false;
        ++revision;
        selected = 0;
    }
    void tick()
    {
        if (!screen) return;
        const uint32_t now = lv_tick_get();
        if (refresh_at && static_cast<int32_t>(now - refresh_at) < 0) return;
        refresh_at = now + 350;
        refresh_snapshots();
        const auto& status = live_status;
        if (waiting_for_scan) {
            if (wifi()) {
                const auto& current = live_wifi;
                if (current.generation != scan_start_generation && !current.scanning)
                    copy_results();
            } else {
                const auto& current = live_ble;
                if (current.generation != scan_start_generation && !current.scanning)
                    copy_results();
            }
        }
        if (waiting_for_scan && status.completed != scan_start_completed && status.last_error) {
            waiting_for_scan = false;
            notify(esp_err_to_name(status.last_error), true);
        }
        if (waiting_for_scan && static_cast<int32_t>(now - scan_deadline) >= 0) {
            waiting_for_scan = false;
            notify("SCAN TIMEOUT / RETRY", true);
        }
        const auto& ble = live_ble;
        const int rssi = wifi() ? (status.wifi.state == connectivity::WifiState::connected
                                       ? status.wifi.rssi : -100)
                               : (ble.peer_connected ? ble.peer_rssi : -100);
        history[history_cursor++ % history.size()] =
            static_cast<int8_t>(std::clamp(rssi, -100, -20));
        render(status, ble);
    }
    void render(const connectivity::Snapshot& status,
                const connectivity::BleDiagnostics& ble)
    {
        char text[384]{};
        lv_obj_set_style_text_font(headline, view == View::overview ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
        const auto& wd = live_wifi;
        const auto name_mode = view == View::target ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
        if (lv_label_get_long_mode(subhead) != name_mode) lv_label_set_long_mode(subhead, name_mode);
        for (auto* row : rows) show(row, false);
        show(body, true);
        for (auto* bar : bars) show(bar, view == View::overview);
        set_text(title, wifi() ? LV_SYMBOL_WIFI " WIFI ANALYZER" :
                                LV_SYMBOL_BLUETOOTH " BLE ANALYZER");
        if (wifi()) {
            if (status.wifi.state == connectivity::WifiState::connected)
                std::snprintf(text, sizeof(text), "%d dBm", status.wifi.rssi);
            else std::snprintf(text, sizeof(text), "%s",
                    !wd.enabled ? "RADIO OFF" : wd.scanning ? "SCANNING" :
                    status.wifi.state == connectivity::WifiState::connecting ? "CONNECTING" :
                    status.wifi.state == connectivity::WifiState::retry_wait ? "RETRY WAIT" :
                    status.wifi.ap_active ? "AP READY" : "STARTING");
        } else std::snprintf(text, sizeof(text), "%s",
                !ble.enabled ? "RADIO OFF" : ble.connecting ? "CONNECTING" :
                ble.peer_connected ? "PEER LINK" : ble.scanning ? "SCANNING" :
                status.ble.connected ? "PHONE LINK" : "ADVERTISING");
        set_text(headline, text);
        std::snprintf(text, sizeof(text), wifi() ? "2.4 GHz   CH %u" : "GATT   MTU %u",
                      wifi() ? wd.channel : status.ble.mtu);
        set_text(subhead, text);
        if (view == View::overview) {
            if (wifi()) {
                char ssid[33];
                printable(ssid, sizeof(ssid), status.wifi.ssid);
                std::snprintf(text, sizeof(text),
                    "%.16s\n%s\nAP clients  %u\nRetries  %lu\nLast err  %ld",
                    ssid[0] ? ssid : "No saved network",
                    status.wifi.address[0] ? status.wifi.address : status.wifi.ap_address,
                    wd.ap_clients, static_cast<unsigned long>(status.wifi.reconnects),
                    static_cast<long>(status.last_error));
            } else std::snprintf(text, sizeof(text),
                    "Phone  %s\nPeer   %s\nSecure %s / %s\nRX %lu  Drop %lu\nLast err  %ld",
                    status.ble.connected ? "LINK" : "none",
                    ble.peer_connected ? "LINK" : "none",
                    status.ble.encrypted ? "yes" : "no", status.ble.bonded ? "bond" : "new",
                    static_cast<unsigned long>(status.ble.received),
                    static_cast<unsigned long>(status.ble.dropped),
                    static_cast<long>(ble.peer_error ? ble.peer_error : status.last_error));
            set_text(body, text);
            for (size_t i = 0; i < bars.size(); ++i) {
                const int level = history[(history_cursor + i) % history.size()];
                const int height = 1 + (level + 100) * 12 / 80;
                lv_obj_set_height(bars[i], height);
                lv_obj_set_y(bars[i], 185 - height);
                lv_obj_set_style_bg_opa(bars[i], level > -100 ? LV_OPA_80 : LV_OPA_20, 0);
            }
            const char* wa[] = {"Scan nearby", "Last scan", "Link details",
                "Traffic / echo", "Reconnect saved", "Disconnect STA",
                wd.enabled ? "Turn radio OFF" : "Turn radio ON", "Local hotspot"};
            const char* ba[] = {"Scan nearby", "Last scan", "Link details",
                "Traffic / echo", "Peer services", "Disconnect peer",
                ble.enabled ? "Turn radio OFF" : "Turn radio ON"};
            std::snprintf(text, sizeof(text), "> %s", wifi() ? wa[choice % 8] : ba[choice % 7]);
            set_text(action, text);
        } else if (view == View::nearby) {
            const uint8_t count = wifi() ? wifi_list.count : ble_list.count;
            const uint16_t total = wifi() ? wifi_list.total_found : ble_list.total_found;
            const bool scanning = wifi() ? wd.scanning : ble.scanning;
            std::snprintf(text, sizeof(text), "%s %u / %u",
                          scanning || waiting_for_scan ? "SCAN" : "FOUND", count, total);
            set_text(headline, text);
            const int32_t error = wifi() ? wd.scan_error : ble.scan_error;
            if (error) std::snprintf(text, sizeof(text), "Scan error %ld", static_cast<long>(error));
            else std::snprintf(text, sizeof(text), "%s",
                   waiting_for_scan ? "Waiting for results" : "dBm / K2 details");
            set_text(subhead, text);
            if (!count) {
                set_text(body, waiting_for_scan ? "Scanning nearby...\nResults appear here\nwhen complete.\n\nHold K2 to leave."
                    : wifi() ? "No scan results.\nK2 starts a scan."
                    : "No scan results.\nK2 starts a scan.\n\nBLE lists nearby\nadvertisers only.");
                set_text(action, waiting_for_scan ? "> SCANNING..." : "> Scan again");
            } else {
                show(body, false);
                const uint8_t start = static_cast<uint8_t>((selected / 3) * 3);
                for (size_t i = 0; i < rows.size() && start + i < count; ++i) {
                    const uint8_t item = start + static_cast<uint8_t>(i);
                    char name[40];
                    if (wifi()) {
                        const auto& n = wifi_list.networks[item];
                        printable(name, sizeof(name), n.ssid[0] ? n.ssid : "[hidden SSID]");
                        fit_width(name, 88);
                        std::snprintf(text, sizeof(text), "%u %.14s\n%d C%u %s",
                              item + 1, name, n.rssi, n.channel, auth_short(n.auth));
                    } else {
                        const auto& n = ble_list.devices[item];
                        printable(name, sizeof(name), n.name[0] ? n.name : n.address);
                        fit_width(name, 88);
                        std::snprintf(text, sizeof(text), "%u %.14s\n%d %s",
                              item + 1, name, n.rssi, n.connectable ? "CONNECT" : "BEACON");
                    }
                    set_text(rows[i], text);
                    lv_obj_set_style_bg_color(rows[i], lv_color_hex(color()), 0);
                    lv_obj_set_style_bg_opa(rows[i], item == selected ? LV_OPA_20 : LV_OPA_TRANSP, 0);
                    lv_obj_set_style_text_color(rows[i], lv_color_hex(item == selected ? kText : kMuted), 0);
                    show(rows[i], true);
                }
                std::snprintf(text, sizeof(text), "> Details  %u / %u", selected + 1, count);
                set_text(action, text);
            }
        } else if (view == View::target) {
            char name[40];
            if (wifi()) {
                printable(name, sizeof(name), wifi_target.ssid);
                set_text(headline, "ACCESS POINT");
                std::snprintf(text, sizeof(text), "%s", name[0] ? name : "[hidden SSID]");
                set_text(subhead, text);
                std::snprintf(text, sizeof(text),
                    "RSSI  %d dBm\nCH %u / %s\nBSSID\n%.9s\n%s\n%s",
                    wifi_target.rssi, wifi_target.channel, auth_name(wifi_target.auth),
                    wifi_target.bssid, tail(wifi_target.bssid, 9),
                    !wifi_target.ssid[0] ? "Needs SSID input" :
                    !supported_auth(wifi_target.auth) ? "Auth unsupported" :
                    wifi_target.auth == 0 ? "No password" : "K2 edit password");
                set_text(action, choice % 2 ? "> Back to list" :
                    !wifi_target.ssid[0] || !supported_auth(wifi_target.auth) ? "> Not connectable" :
                    wifi_target.auth == 0 ? "> Connect open AP" : "> Enter password");
            } else {
                printable(name, sizeof(name), ble_target.name);
                set_text(headline, "BLE DEVICE");
                std::snprintf(text, sizeof(text), "%s", name[0] ? name : "Unnamed device");
                set_text(subhead, text);
                std::snprintf(text, sizeof(text),
                    "RSSI  %d dBm\n%s address\n%.9s\n%s\n%s\nGATT discovery",
                    ble_target.rssi, ble_target.address_type ? "Random" : "Public",
                    ble_target.address, tail(ble_target.address, 9),
                    ble_target.connectable ? "Connectable" : "Advertise only");
                set_text(action, choice % 2 ? "> Back to list" :
                    ble_target.connectable ? "> Connect device" : "> Beacon only");
            }
            set_text(body, text);
        } else if (view == View::link) {
            set_text(headline, "LINK DETAILS");
            std::snprintf(text, sizeof(text), "Page %u / %u", page + 1, wifi() ? 3 : 2);
            set_text(subhead, text);
            if (wifi()) {
                char name[40];
                printable(name, sizeof(name), page == 1 ? status.wifi.ap_ssid : status.wifi.ssid);
                if (page == 0) std::snprintf(text, sizeof(text),
                    "STA %s\n%.16s\n%.16s\nIP %s\nReason %u\nRetry %lu",
                    connectivity::wifi_state_name(status.wifi.state), name, tail(name, 16),
                    status.wifi.address[0] ? status.wifi.address : "--", status.wifi.disconnect_reason,
                    static_cast<unsigned long>(status.wifi.reconnects));
                else if (page == 1) std::snprintf(text, sizeof(text),
                    "LOCAL HOTSPOT\n%.16s\n%.16s\n%s\nClients %u\nChannel %u",
                    name, tail(name, 16), status.wifi.ap_address, wd.ap_clients, wd.channel);
                else std::snprintf(text, sizeof(text),
                    "STA BSSID\n%.9s\n%s\nRadio %s\nStation %s\nScan gen %lu",
                    wd.bssid, tail(wd.bssid, 9), wd.enabled ? "ON" : "OFF",
                    wd.station_enabled ? "ON" : "OFF", static_cast<unsigned long>(wd.generation));
            } else if (page == 0) std::snprintf(text, sizeof(text),
                    "PHONE / SERVER\nLink %s  MTU %u\nEncrypted %s\nBonded %s\nNotify %s\nRX %lu / lost %lu",
                    status.ble.connected ? "ON" : "OFF", status.ble.mtu,
                    status.ble.encrypted ? "yes" : "no", status.ble.bonded ? "yes" : "no",
                    status.ble.subscribed ? "yes" : "no",
                    static_cast<unsigned long>(status.ble.received),
                    static_cast<unsigned long>(status.ble.dropped));
            else {
                char name[32];
                printable(name, sizeof(name), ble.peer_name);
                std::snprintf(text, sizeof(text),
                    "PEER / CLIENT\n%.16s\n%.9s\n%s\nRSSI %d MTU %u\nErr %ld / Svc %u",
                    name[0] ? name : "--", ble.peer_address, tail(ble.peer_address, 9),
                    ble.peer_rssi, ble.peer_mtu, static_cast<long>(ble.peer_error), ble.service_count);
            }
            set_text(body, text);
            set_text(action, "> Next page");
        } else if (view == View::traffic) {
            const auto& traffic = live_traffic;
            set_text(headline, page ? "LAST ECHO" : "TRAFFIC");
            set_text(subhead, "Shared HTTP + BLE");
            if (page) {
                char echo[65];
                printable(echo, sizeof(echo), traffic.echo);
                std::snprintf(text, sizeof(text), "%.16s\n%.16s\n%.16s\n%.16s\n\nK2 counters",
                    echo[0] ? echo : "No echo received", tail(echo, 16), tail(echo, 32), tail(echo, 48));
            } else std::snprintf(text, sizeof(text),
                    "HTTP %lu BLE %lu\nRX %lu bytes\nTX %lu bytes\n%s / %.10s\nID %ld  %s\nDone %lu / bad %lu",
                    static_cast<unsigned long>(traffic.http_requests),
                    static_cast<unsigned long>(traffic.ble_requests),
                    static_cast<unsigned long>(traffic.rx_bytes), static_cast<unsigned long>(traffic.tx_bytes),
                    traffic.last_transport, traffic.last_operation, static_cast<long>(traffic.last_request_id),
                    traffic.last_ok ? "OK" : "--", static_cast<unsigned long>(status.completed),
                    static_cast<unsigned long>(status.rejected));
            set_text(body, text);
            set_text(action, page ? "> Show counters" : "> Show echo text");
        } else if (view == View::services) {
            set_text(headline, "PEER SERVICES");
            std::snprintf(text, sizeof(text), "%u cached UUIDs", ble.service_count);
            set_text(subhead, text);
            if (!ble.service_count) {
                set_text(body, ble.connecting ? "Connecting...\nDiscovering GATT\nservices."
                           : ble.peer_connected ? "Discovery pending\nor no services."
                           : "Connect a nearby\nBLE device first.\n\nServices are read\nwithout writing to\nthe remote device.");
                set_text(action, "> Waiting for peer");
            } else {
                const uint8_t index = page % ble.service_count;
                const char* uuid = ble.services[index];
                std::snprintf(text, sizeof(text), "SERVICE %u / %u\n%.16s\n%.16s\n%.7s\n\nRead-only discovery",
                    index + 1, ble.service_count, uuid, tail(uuid, 16), tail(uuid, 32));
                set_text(body, text);
                set_text(action, "> Next service");
            }
        } else if (view == View::credentials) {
            set_text(headline, "LOCAL AP");
            set_text(subhead, "Private / on-device");
            char secret[17]{};
            if (!connectivity::copy_local_ap_password(secret, sizeof(secret)))
                std::snprintf(secret, sizeof(secret), "%s", "Unavailable");
            std::snprintf(text, sizeof(text), "%.16s\n%s\nIP %s\nPASSWORD\n%.8s\n%s",
                status.wifi.ap_ssid, tail(status.wifi.ap_ssid, 16),
                status.wifi.ap_address, secret, tail(secret, 8));
            set_text(body, text);
            wipe(secret, sizeof(secret));
            set_text(action, "> Back");
        } else if (view == View::password_group || view == View::password_character) {
            set_text(headline, "PASSWORD");
            char mask[17]{};
            std::memset(mask, '*', std::min<size_t>(16, std::strlen(password)));
            std::snprintf(text, sizeof(text), "%s %u", mask, static_cast<unsigned>(std::strlen(password)));
            set_text(subhead, text);
            if (view == View::password_character) {
                const char value = kGroups[group][character];
                std::snprintf(text, sizeof(text),
                    "GROUP %s\nCHAR %u / %u\n\n      [ %c ]\n\nK2 adds character",
                    kGroupNames[group], character + 1,
                    static_cast<unsigned>(std::strlen(kGroups[group])), value);
                set_text(action, "> Add character");
            } else {
                const char* names[] = {"a-m", "n-z", "A-M", "N-Z", "0-9", "Punctuation !-/",
                    "Symbols :-@", "Symbols [-grave", "Symbols {-~", "SPACE",
                    "Delete last", "Save + connect", "Cancel"};
                std::snprintf(text, sizeof(text),
                    "Select a group\n\n> %s\n  %s\n  %s\n\n%u chars / max 64",
                    names[group], names[(group + 1) % 13], names[(group + 2) % 13],
                    static_cast<unsigned>(std::strlen(password)));
                set_text(action, group == 11 ? "> Save + connect" :
                                 group == 12 ? "> Discard password" : "> Select");
            }
            set_text(body, text);
        } else if (view == View::confirm_radio_off) {
            set_text(headline, "RADIO OFF?");
            set_text(subhead, "Confirm on device");
            set_text(body, wifi() ? "Stops WiFi and\nlocal hotspot.\nSaved credentials\nare kept.\n\nK1 cancel / K2 off"
                                  : "Disconnects phone\nand nearby peer.\nPairing is kept.\n\nK1 cancel / K2 off");
            set_text(action, "> Confirm OFF");
        }
        if (message[0] && static_cast<int32_t>(message_until - lv_tick_get()) > 0)
            set_text(action, message);
        else {
            message[0] = '\0';
            lv_obj_set_style_text_color(action, lv_color_hex(color()), 0);
        }
    }
    void next()
    {
        ++revision;
        switch (view) {
        case View::overview: choice = (choice + 1) % (wifi() ? 8 : 7); break;
        case View::nearby: {
            const uint8_t count = wifi() ? wifi_list.count : ble_list.count;
            if (count) selected = (selected + 1) % count;
            break;
        }
        case View::target: choice = (choice + 1) % 2; break;
        case View::link: page = (page + 1) % (wifi() ? 3 : 2); break;
        case View::traffic: page = (page + 1) % 2; break;
        case View::services: ++page; break;
        case View::password_group: group = (group + 1) % 13; break;
        case View::password_character:
            character = (character + 1) % std::strlen(kGroups[group]); break;
        case View::credentials: case View::confirm_radio_off: enter(View::overview); break;
        }
        message[0] = '\0';
        refresh_at = 0;
    }
    bool select(bool back)
    {
        if (back) {
            switch (view) {
            case View::overview: return true;
            case View::password_character: enter(View::password_group); break;
            case View::password_group:
                wipe(password, sizeof(password)); enter(View::target); choice = 0; break;
            case View::target: enter(View::nearby); break;
            default: enter(View::overview); choice = 0; break;
            }
            refresh_at = 0;
            return false;
        }
        switch (view) {
        case View::overview:
            switch (choice) {
            case 0: scan(); break;
            case 1: copy_results(); enter(View::nearby); break;
            case 2: enter(View::link); break;
            case 3: enter(View::traffic); break;
            case 4:
                if (wifi()) command(connectivity::ControlAction::wifi_reconnect);
                else enter(View::services);
                break;
            case 5: command(wifi() ? connectivity::ControlAction::wifi_disconnect
                                  : connectivity::ControlAction::ble_disconnect); break;
            case 6:
                if (wifi() ? live_wifi.enabled : live_ble.enabled)
                    enter(View::confirm_radio_off);
                else command(wifi() ? connectivity::ControlAction::wifi_enable
                                    : connectivity::ControlAction::ble_enable);
                break;
            case 7: enter(View::credentials); break;
            }
            break;
        case View::nearby: {
            const uint8_t count = wifi() ? wifi_list.count : ble_list.count;
            if (!count) {
                if (!waiting_for_scan) scan();
                break;
            }
            if (selected >= count) { notify("RESULT EXPIRED", true); break; }
            if (wifi()) wifi_target = wifi_list.networks[selected];
            else ble_target = ble_list.devices[selected];
            enter(View::target); choice = 0;
            break;
        }
        case View::target: {
            if (choice % 2) { enter(View::nearby); break; }
            connectivity::ControlRequest request{};
            if (wifi()) {
                if (!wifi_target.ssid[0]) { notify("HIDDEN: USE CLIENT", true); break; }
                if (!supported_auth(wifi_target.auth)) { notify("AUTH UNSUPPORTED", true); break; }
                if (wifi_target.auth != 0) {
                    wipe(password, sizeof(password)); group = character = 0;
                    enter(View::password_group); break;
                }
                request.action = connectivity::ControlAction::wifi_connect;
                std::snprintf(request.wifi.ssid, sizeof(request.wifi.ssid), "%s", wifi_target.ssid);
            } else {
                if (!ble_target.connectable) { notify("BEACON ONLY", true); break; }
                request.action = connectivity::ControlAction::ble_connect;
                std::snprintf(request.address, sizeof(request.address), "%s", ble_target.address);
                request.address_type = ble_target.address_type;
            }
            enter(View::link);
            if (!wifi()) page = 1;
            queue(request);
            break;
        }
        case View::password_character: {
            const size_t length = std::strlen(password);
            if (length >= sizeof(password) - 1) { notify("PASSWORD FULL", true); break; }
            password[length] = kGroups[group][character];
            password[length + 1] = '\0';
            enter(View::password_group);
            break;
        }
        case View::password_group:
            if (group < kGroups.size()) {
                character = 0; enter(View::password_character);
            } else if (group == 10) {
                const size_t length = std::strlen(password);
                if (length) password[length - 1] = '\0';
            } else if (group == 12) {
                wipe(password, sizeof(password)); enter(View::target); choice = 0;
            } else {
                connectivity::ControlRequest request{};
                request.action = connectivity::ControlAction::wifi_connect;
                std::snprintf(request.wifi.ssid, sizeof(request.wifi.ssid), "%s", wifi_target.ssid);
                std::snprintf(request.wifi.password, sizeof(request.wifi.password), "%s", password);
                if (std::strlen(password) < 8 || !connectivity::valid_credentials(request.wifi)) {
                    wipe(request.wifi.password, sizeof(request.wifi.password));
                    notify("USE 8-63 / 64 HEX", true); break;
                }
                const esp_err_t result = connectivity::request_control(request);
                wipe(request.wifi.password, sizeof(request.wifi.password));
                if (result == ESP_OK) {
                    wipe(password, sizeof(password)); enter(View::link); notify("QUEUED");
                } else notify(esp_err_to_name(result), true);
            }
            break;
        case View::link: page = (page + 1) % (wifi() ? 3 : 2); break;
        case View::traffic: page = (page + 1) % 2; break;
        case View::services: ++page; break;
        case View::credentials: enter(View::overview); choice = 0; break;
        case View::confirm_radio_off:
            enter(View::overview); choice = 0;
            command(wifi() ? connectivity::ControlAction::wifi_disable
                           : connectivity::ControlAction::ble_disable);
            break;
        }
        refresh_at = 0;
        return false;
    }
    ~Impl()
    {
        wipe(password, sizeof(password));
        if (screen) lv_obj_delete(screen);
    }
};

RadioUi::RadioUi() = default;
RadioUi::~RadioUi() = default;
lv_obj_t* RadioUi::open(Kind kind)
{
    impl_.reset(new (std::nothrow) Impl);
    if (!impl_) return nullptr;
    impl_->kind = kind;
    if (!impl_->create()) { impl_.reset(); return nullptr; }
    impl_->tick();
    return impl_->screen;
}
void RadioUi::close() { impl_.reset(); }
void RadioUi::next()
{
    if (impl_) {
        impl_->next();
        impl_->tick();
    }
}
void RadioUi::press()
{
    if (impl_) {
        impl_->pressed_revision = impl_->revision;
        impl_->press_armed = true;
    }
}
bool RadioUi::select(bool long_press)
{
    if (!impl_) return true;
    const bool stable = impl_->press_armed && impl_->pressed_revision == impl_->revision;
    impl_->press_armed = false;
    if (!long_press && !stable) {
        impl_->notify("VIEW CHANGED / RETRY", true);
        impl_->refresh_at = 0;
        impl_->tick();
        return false;
    }
    const bool leave = impl_->select(long_press);
    // The control handler frame has returned before snapshot/render work.
    if (!leave) impl_->tick();
    return leave;
}
void RadioUi::update() { if (impl_) impl_->tick(); }

#ifdef M5_STICKS3_RADIO_SMOKE_TEST
void RadioUi::smoke_step(uint8_t step)
{
    if (!impl_) return;
    switch (step) {
    case 0: impl_->enter(View::overview); break;
    case 1: impl_->scan(); break;
    case 2: impl_->copy_results(); impl_->enter(View::nearby); break;
    case 3:
        if ((impl_->wifi() && impl_->wifi_list.count) ||
            (!impl_->wifi() && impl_->ble_list.count)) {
            impl_->enter(View::nearby);
            impl_->select(false); // details only; never connect an unknown peer
        }
        break;
    case 4: impl_->enter(View::traffic); break;
    case 5: impl_->enter(View::link); break;
    case 6: impl_->enter(View::services); break;
    default: break;
    }
    impl_->refresh_at = 0;
    impl_->tick();
}
void RadioUi::capture(const char* name)
{
#if LV_USE_SNAPSHOT
    if (!impl_ || !impl_->screen) return;
    lv_obj_update_layout(impl_->screen);
    constexpr uint32_t width = 135, height = 240;
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    const uint32_t capacity = stride * height + LV_DRAW_BUF_ALIGN;
    void* memory = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) { ESP_LOGE("radio_ui", "snapshot allocation failed"); return; }
    lv_draw_buf_t buffer{};
    if (lv_draw_buf_init(&buffer, width, height, LV_COLOR_FORMAT_RGB565, stride,
                         memory, capacity) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(impl_->screen, LV_COLOR_FORMAT_RGB565, &buffer) != LV_RESULT_OK) {
        heap_caps_free(memory);
        ESP_LOGE("radio_ui", "snapshot rendering failed");
        return;
    }
    std::printf("RADIO_FRAME %s %lu %lu %lu %lu\n", name,
                static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                static_cast<unsigned long>(stride), static_cast<unsigned long>(stride * height));
    char line[129];
    constexpr char digits[] = "0123456789abcdef";
    for (uint32_t offset = 0; offset < stride * height; offset += 64) {
        const uint32_t bytes = std::min<uint32_t>(64, stride * height - offset);
        for (uint32_t i = 0; i < bytes; ++i) {
            line[i * 2] = digits[buffer.data[offset + i] >> 4];
            line[i * 2 + 1] = digits[buffer.data[offset + i] & 15];
        }
        line[bytes * 2] = '\0';
        std::printf("RADIO_DATA %lu %s\n", static_cast<unsigned long>(offset), line);
    }
    std::printf("RADIO_END %s\n", name);
    heap_caps_free(memory);
#else
    ESP_LOGW("radio_ui", "enable LV_USE_SNAPSHOT for smoke screenshots");
#endif
}
#endif
} // namespace app_modules
