#include "debug_probe.hpp"
#include "dap_session.hpp"
#include "probe_usb.hpp"
#include "app_task.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
extern "C" {
#include "DAP_config.h"
#include "DAP.h"
}

namespace debug_probe {
namespace {
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot state;
std::atomic<Mode> wanted{Mode::off};
std::atomic<uint32_t> generation{0}, clock_limit{1000000};
std::atomic<bool> initialized{false};
uint32_t command_generation = 0, requested_clock = 1000000;
int64_t deadline_us = 0, last_abort_poll_us = 0;
int tcp_abort_socket = -1;
std::atomic<bool> transfer_busy{false}, abort_requested{false};
char serial[13]{};

BleSession ble;
std::atomic<bool> ble_lost{false};

template<class F> void update(F f) {
    portENTER_CRITICAL(&state_lock); f(state); portEXIT_CRITICAL(&state_lock);
}
void clear_ble() {
    portENTER_CRITICAL(&state_lock); ble.reset(); portEXIT_CRITICAL(&state_lock);
    ble_lost.store(false);
}
void reset_session() {
    DAP_Setup(); // releases all target pins, resets transfer configuration
    update([](Snapshot& s) { s.swd = false; s.running = false; s.connected = false; });
}
Packet execute(const Packet& request) {
    Packet reply{};
    if (command_generation != generation.load() || wanted.load() == Mode::off) return reply;
    const int64_t start = esp_timer_get_time();
    abort_requested.store(false);
    transfer_busy.store(request.data[0] == 5 || request.data[0] == 6);
    deadline_us = start + 200000; // bounds WAIT/match loops even for hostile hosts
    if (!valid_request(request.data, request.size)) {
        reply.data[0] = 0xff; reply.size = 1;
        update([](Snapshot& s) { ++s.errors; });
    } else if (request.data[0] == 0x07) {
        DAP_TransferAbort = 1; // CMSIS-DAP TransferAbort has no response
    } else if (request.data[0] == 0x11 && read32(request.data + 1) < 1000) {
        reply.data[0] = 0x11; reply.data[1] = 0xff; reply.size = 2;
    } else {
        uint8_t padded[kPacketSize]{};
        std::memcpy(padded, request.data, request.size);
        // SWJ_Pins can otherwise poll for three seconds in the reference engine.
        if (padded[0] == 0x10) write32(padded + 3, std::min<uint32_t>(read32(padded + 3), 100000U));
        // Apply a changed UI ceiling without changing the host's requested rate.
        DAP_Data.clock_delay = probe_set_clock(requested_clock);
        reply.size = DAP_ProcessCommand(padded, reply.data) & 0xffff;
        if (reply.size > kPacketSize) { reply = {}; reply.data[0] = 0xff; reply.size = 1; }
    }
    transfer_busy.store(false);
    deadline_us = 0;
    update([&](Snapshot& s) {
        ++s.packets; s.rx_bytes += request.size; s.tx_bytes += reply.size;
        s.last_command = request.data[0];
        s.last_us = esp_timer_get_time() - start;
        s.swd = DAP_Data.debug_port == DAP_PORT_SWD;
        if (reply.data[0] == 5 && reply.size >= 3) s.last_ack = reply.data[2];
        if (reply.data[0] == 6 && reply.size >= 4) s.last_ack = reply.data[3];
        if ((reply.data[0] == 5 || reply.data[0] == 6) && s.last_ack != 1) ++s.errors;
    });
    return reply;
}

class Worker final : public AppTask {
public:
    Worker() : AppTask("debug_probe", 6144, 4, 0) {}
private:
    Mode mode = Mode::off;
    uint32_t session = 0;
    int server = -1, client = -1;
    uint8_t header[8]{};
    size_t header_used = 0, body_used = 0;
    Packet input{}, output{};
    uint8_t outgoing[8 + kPacketSize]{};
    size_t outgoing_size = 0, outgoing_sent = 0;
    int64_t partial_since = 0, sending_since = 0;

    void close_client() {
        tcp_abort_socket = -1;
        if (client >= 0) { shutdown(client, SHUT_RDWR); close(client); client = -1; }
        header_used = body_used = outgoing_size = outgoing_sent = 0;
        partial_since = sending_since = 0;
        reset_session();
    }
    bool stop_mode() {
        update([](Snapshot& s) { s.ready = false; });
        close_client();
        if (server >= 0) { close(server); server = -1; }
        clear_ble();
        if (mode == Mode::usb) {
            const auto err = usb_stop();
            if (err != ESP_OK) { update([&](Snapshot& s) { s.error = err; }); return false; }
        }
        output = {};
        mode = Mode::off;
        update([](Snapshot& s) { s.mode = Mode::off; s.connected = false; });
        return true;
    }
    esp_err_t listen_tcp() {
        server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server < 0) return ESP_FAIL;
        int one = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_port = htons(kTcpPort); addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(server, 1) ||
            fcntl(server, F_SETFL, O_NONBLOCK) < 0) {
            close(server); server = -1; return ESP_FAIL;
        }
        return ESP_OK;
    }
    void start_mode(Mode next) {
        mode = next;
        gpio_config_t pins{};
        pins.pin_bit_mask = (1ULL << kSwclk) | (1ULL << kSwdio) | (1ULL << kReset);
        pins.mode = GPIO_MODE_INPUT;
        const auto pin_error = gpio_config(&pins);
        if (pin_error != ESP_OK) {
            update([&](Snapshot& s) { s.mode = mode; s.ready = false; s.error = pin_error; });
            return;
        }
        update([&](Snapshot& s) { s = {}; s.mode = mode; s.limit_hz = clock_limit.load(); });
        // DAP_Setup publishes the default host clock after applying the UI
        // ceiling. Preserve it when a low-speed page is resumed or reopened.
        reset_session();
        esp_err_t err = ESP_OK;
        if (mode == Mode::usb) err = usb_start();
        if (mode == Mode::wifi) err = listen_tcp();
        update([&](Snapshot& s) { s.ready = err == ESP_OK; s.error = err; });
    }
    void poll_tcp() {
        // Drain additional connections immediately; exactly one host owns SWD.
        const int peer = accept(server, nullptr, nullptr);
        if (peer >= 0) {
            if (client >= 0) close(peer);
            else {
                client = peer;
                tcp_abort_socket = peer;
                if (fcntl(client, F_SETFL, O_NONBLOCK) < 0) { close_client(); return; }
                int one = 1;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                int idle = 30, interval = 5, count = 3;
                setsockopt(client, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                setsockopt(client, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
                setsockopt(client, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
                reset_session(); update([](Snapshot& s) { s.connected = true; });
            }
        }
        if (client < 0) return;
        const int64_t now = esp_timer_get_time();
        if ((partial_since && now - partial_since > 2000000) ||
            (sending_since && now - sending_since > 2000000)) { close_client(); return; }
        if (outgoing_size) {
            const int n = send(client, outgoing + outgoing_sent, outgoing_size - outgoing_sent, 0);
            if (n > 0) outgoing_sent += n;
            else if (n == 0 || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) { close_client(); return; }
            if (outgoing_sent == outgoing_size) { outgoing_size = outgoing_sent = 0; sending_since = 0; }
            return;
        }
        uint8_t* target = header_used < 8 ? header + header_used : input.data + body_used;
        const size_t capacity = header_used < 8 ? 8 - header_used : input.size - body_used;
        const int n = recv(client, target, capacity, 0);
        if (n == 0) { close_client(); return; }
        if (n < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) close_client();
            return;
        }
        if (!partial_since) partial_since = now;
        if (header_used < 8) {
            header_used += n;
            if (header_used == 8 && !tcp_length(header, input.size)) { close_client(); return; }
            return;
        }
        body_used += n;
        if (body_used == input.size) {
            const auto reply = execute(input);
            header_used = body_used = 0; partial_since = 0;
            if (reply.size) {
                tcp_header(outgoing, reply.size);
                std::memcpy(outgoing + 8, reply.data, reply.size);
                outgoing_size = reply.size + 8; sending_since = now;
            }
        }
    }
    void poll_usb() {
        if (usb_take_disconnect()) { reset_session(); output = {}; }
        update([](Snapshot& s) { s.connected = usb_connected(); });
        if (output.size) {
            if (usb_send(output)) { output = {}; sending_since = 0; }
            else if (esp_timer_get_time() - sending_since > 2000000) {
                // Reenumerate to clear stale responses and endpoint ownership.
                generation.fetch_add(1);
            }
            return;
        }
        Packet request{};
        if (usb_receive(request)) { output = execute(request); sending_since = esp_timer_get_time(); }
    }
    void poll_ble() {
        if (ble_lost.exchange(false)) reset_session();
        Packet request{};
        BleSession::Token token;
        portENTER_CRITICAL(&state_lock);
        const bool available = ble.take(session, request, token);
        portEXIT_CRITICAL(&state_lock);
        if (!available) return;
        const auto response = execute(request);
        portENTER_CRITICAL(&state_lock);
        if (ble.complete(token, response)) state.connected = true;
        portEXIT_CRITICAL(&state_lock);
    }
    void main() override {
        while (!should_exit()) {
            const uint32_t selected_generation = generation.load();
            const Mode selected = wanted.load();
            if (session != selected_generation || mode != selected) {
                if (!stop_mode()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
                session = selected_generation; command_generation = session;
                if (selected != Mode::off) start_mode(selected);
            }
            if (snapshot().ready) {
                if (mode == Mode::usb) poll_usb();
                if (mode == Mode::wifi) poll_tcp();
                if (mode == Mode::ble) poll_ble();
            }
            vTaskDelay(pdMS_TO_TICKS(mode == Mode::off ? 10 : 1));
        }
        // Retain this task while teardown needs retry; owner sees stop timeout.
        while (!stop_mode()) vTaskDelay(pdMS_TO_TICKS(50));
    }
};
Worker worker;
} // namespace

esp_err_t initialize() {
    if (initialized.load()) return ESP_OK;
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    snprintf(serial,sizeof(serial),"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if (!worker.start()) return ESP_ERR_NO_MEM;
    initialized.store(true); return ESP_OK;
}
esp_err_t deinitialize() {
    select_mode(Mode::off);
    if (!worker.stop(pdMS_TO_TICKS(1500))) return ESP_ERR_TIMEOUT;
    initialized.store(false); return ESP_OK;
}
void select_mode(Mode mode) {
    if (wanted.exchange(mode) != mode) generation.fetch_add(1);
}
void set_clock_limit(uint32_t hz) { clock_limit.store(std::clamp<uint32_t>(hz, 100000U, 2000000U)); }
Snapshot snapshot() {
    portENTER_CRITICAL(&state_lock); const auto result = state; portEXIT_CRITICAL(&state_lock); return result;
}
const char* mode_name(Mode mode) {
    switch (mode) { case Mode::usb: return "USB DAP"; case Mode::wifi: return "WIFI DAP";
        case Mode::ble: return "BLE DAP"; default: return "OFF"; }
}
void request_transfer_abort(Mode source) {
    if (wanted.load() == source && transfer_busy.load()) abort_requested.store(true);
}
bool ble_write(uint16_t connection, const uint8_t* data, size_t size) {
    if (wanted.load() != Mode::ble) return false;
    if (size == 5 && data[2] == 0 && data[3] == 1 && data[4] == 7) {
        request_transfer_abort(Mode::ble); return true;
    }
    portENTER_CRITICAL(&state_lock);
    const bool ok = state.mode == Mode::ble && state.ready &&
        ble.write(connection, data, size, generation.load());
    portEXIT_CRITICAL(&state_lock);
    return ok;
}
size_t ble_read(uint16_t connection, uint8_t* output, size_t capacity) {
    if (wanted.load() != Mode::ble) return 0;
    portENTER_CRITICAL(&state_lock);
    const size_t size = state.mode == Mode::ble && state.ready ? ble.read(connection, output, capacity) : 0;
    portEXIT_CRITICAL(&state_lock);
    return size;
}
void ble_disconnect(uint16_t connection) {
    portENTER_CRITICAL(&state_lock);
    if (ble.disconnect(connection)) ble_lost.store(true);
    portEXIT_CRITICAL(&state_lock);
}
} // namespace debug_probe

extern "C" uint32_t probe_set_clock(uint32_t hz) {
    using namespace debug_probe;
    requested_clock = hz;
    const uint32_t applied = std::min(std::max<uint32_t>(hz, 1000U), clock_limit.load());
    update([&](Snapshot& s) { s.clock_hz = applied; s.limit_hz = clock_limit.load(); });
    // Full half-period delay plus GPIO/instruction overhead: physical SWCLK is
    // never faster than requested. UI shows configured rate, not a measurement.
    return (CPU_CLOCK / 2 + applied - 1) / applied;
}
extern "C" void probe_port_off(void) {
    for (const auto pin : {GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8}) {
        gpio_set_direction(pin, GPIO_MODE_INPUT); gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
}
extern "C" void probe_port_swd(void) {
    gpio_set_level(GPIO_NUM_6, 1); gpio_set_level(GPIO_NUM_7, 1); gpio_set_level(GPIO_NUM_8, 1);
    gpio_set_direction(GPIO_NUM_6, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(GPIO_NUM_7, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(GPIO_NUM_8, GPIO_MODE_INPUT_OUTPUT_OD);
}
extern "C" uint8_t probe_serial(char* text) { std::memcpy(text, debug_probe::serial, 13); return 13; }
extern "C" uint8_t probe_reset(void) {
    gpio_set_level(GPIO_NUM_8, 0); vTaskDelay(pdMS_TO_TICKS(10)); gpio_set_level(GPIO_NUM_8, 1); return 1;
}
extern "C" int probe_cancelled(void) {
    using namespace debug_probe;
    const int64_t now = esp_timer_get_time();
    const Mode mode = wanted.load();
    // The worker is the only TCP reader. During a long transfer it may consume
    // an out-of-band abort frame, leaving all normal pipelined frames untouched.
    if (mode == Mode::wifi && tcp_abort_socket >= 0 && now - last_abort_poll_us >= 1000) {
        last_abort_poll_us = now;
        uint8_t bytes[8 + kPacketSize];
        const int n = recv(tcp_abort_socket, bytes, sizeof(bytes), MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return true;
        if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) return true;
        uint16_t length = 0;
        if (n >= 9 && tcp_length(bytes, length) && bytes[8] == 7 && n >= 8 + length) {
            (void)recv(tcp_abort_socket, bytes, 8 + length, MSG_DONTWAIT);
            abort_requested.store(true);
        }
    }
    return command_generation != generation.load() || mode == Mode::off ||
        (deadline_us && now >= deadline_us) || ble_lost.load() || abort_requested.load() ||
        (mode == Mode::usb && !usb_connected());
}
extern "C" void probe_host_status(unsigned kind, unsigned value) {
    if (kind == 1) debug_probe::update([&](debug_probe::Snapshot& s) { s.running = value; });
}
