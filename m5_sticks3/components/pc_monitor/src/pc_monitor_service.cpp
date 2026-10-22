#include "pc_monitor_service.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"

namespace pc_monitor {
namespace {
constexpr char tag[] = "pc_monitor";
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot public_state{};
uint32_t public_received_at = 0;
uint32_t now_ms() { return uint32_t(esp_timer_get_time() / 1000); }
} // namespace

Service::~Service() { deinitialize(); }

void Service::publish()
{
    state_.stats = latest_.stats();
    state_.received = latest_.received();
    portENTER_CRITICAL(&state_lock);
    public_state = state_;
    public_received_at = received_at_;
    portEXIT_CRITICAL(&state_lock);
}

void Service::close_transport()
{
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    latest_.reset();
    received_at_ = 0;
    state_.listening = false;
}

void Service::failed(int error)
{
    close_transport();
    state_.error = error;
    retry_at_ = now_ms() + 1000;
    ESP_LOGW(tag, "UDP transport failed: errno=%d", error);
}

void Service::open_transport()
{
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) { failed(errno); return; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(udp_port);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) { failed(errno); return; }
    state_.listening = true;
    state_.error = 0;
    ESP_LOGI(tag, "PC monitor UDP listening on %u", udp_port);
}

void Service::poll()
{
    for (unsigned i = 0; i < 4; ++i) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int count = recvfrom(socket_, packet_, sizeof(packet_), MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) failed(errno);
            return;
        }
        Stats sample{};
        // A full buffer can be a truncated datagram; valid v1 is strictly smaller.
        if (count <= 0 || size_t(count) >= sizeof(packet_) ||
            !parse_sample(std::string_view(packet_, size_t(count)), sample)) {
            ++state_.rejected;
            continue;
        }
        const auto now = now_ms();
        if (!latest_.accept(sample, now)) {
            ++state_.out_of_order;
            continue;
        }
        received_at_ = now;
        ++state_.accepted;
        char ack[112]{};
        const int length = std::snprintf(ack, sizeof(ack),
            "{\"v\":1,\"session\":\"%s\",\"seq\":%lu,\"ack\":true}",
            sample.session, static_cast<unsigned long>(sample.seq));
        const int sent = sendto(socket_, ack, size_t(length), MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&peer), peer_size);
        state_.error = sent == length ? 0 : sent < 0 ? errno : EIO;
    }
}

void Service::process(bool wifi_enabled)
{
    if (!started_) { retry_at_ = now_ms(); started_ = true; }
    if (!wifi_enabled) {
        if (socket_ >= 0) close_transport();
        state_.error = 0;
        retry_at_ = now_ms();
    } else {
        if (socket_ < 0 && int32_t(now_ms() - retry_at_) >= 0) open_transport();
        if (socket_ >= 0) poll();
    }
    publish();
}

void Service::deinitialize()
{
    if (!started_) return;
    close_transport();
    state_ = {};
    started_ = false;
    publish();
}

Snapshot snapshot()
{
    Snapshot result;
    uint32_t at;
    portENTER_CRITICAL(&state_lock);
    result = public_state;
    at = public_received_at;
    portEXIT_CRITICAL(&state_lock);
    result.age_ms = result.received ? now_ms() - at : 0;
    result.stale = !result.listening || !result.received || result.age_ms >= stale_after_ms;
    return result;
}
} // namespace pc_monitor
