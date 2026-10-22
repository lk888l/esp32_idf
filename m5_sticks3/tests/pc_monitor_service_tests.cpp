#include "pc_monitor_service.hpp"
#include "platform.hpp"
#include <cassert>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
const std::string packet = R"({"v":1,"session":"s3-test","seq":1,"host":"PC","cpu_pct":37.5,"ram_used":8589934592,"ram_total":34359738368,"gpu_name":"GPU","gpu_pct":81,"vram_used":4294967296,"vram_total":12884901888,"gpu_temp_c":55})";
std::string sequence(unsigned seq)
{
    auto result = packet;
    result.replace(result.find("\"seq\":1"), 7, "\"seq\":" + std::to_string(seq));
    return result;
}
} // namespace

int main()
{
    using namespace pc_monitor;
    const int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(udp >= 0);
    sockaddr_in target{};
    target.sin_family = AF_INET; target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(udp_port);
    assert(connect(udp, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);
    auto send = [&](const std::string& value) {
        assert(::send(udp, value.data(), value.size(), 0) == ssize_t(value.size()));
    };
    auto ack = [&] {
        char buffer[128]{};
        const auto count = recv(udp, buffer, sizeof(buffer), MSG_DONTWAIT);
        return count < 0 ? std::string{} : std::string(buffer, size_t(count));
    };
    Service service;
    service.process(false);
    assert(!snapshot().listening && snapshot().stale);
    service.process(true);
    assert(snapshot().listening && !snapshot().received && !snapshot().error);
    send(packet); service.process(true);
    assert(ack() == R"({"v":1,"session":"s3-test","seq":1,"ack":true})");
    auto state = snapshot();
    assert(state.received && !state.stale && state.accepted == 1 && state.stats.cpu_pct == 37.5f);
    send(packet); service.process(true);
    assert(ack().empty() && snapshot().out_of_order == 1 && snapshot().accepted == 1);
    send("{}"); send(std::string(1024, ' ')); send(packet + std::string(2000, ' '));
    service.process(true);
    assert(ack().empty() && snapshot().rejected == 3 && snapshot().stats.seq == 1);
    fake::now += 2'999'000;
    assert(!snapshot().stale && snapshot().age_ms == 2999);
    fake::now += 1000;
    assert(snapshot().stale && snapshot().age_ms == 3000);
    // Four datagrams per turn; a flood cannot monopolize the application loop.
    for (unsigned i = 2; i < 8; ++i) send(sequence(i));
    service.process(true);
    assert(snapshot().accepted == 5 && snapshot().stats.seq == 5 && !snapshot().stale);
    service.process(true);
    assert(snapshot().accepted == 7 && snapshot().stats.seq == 7);
    service.process(false);
    assert(!snapshot().listening && !snapshot().received && snapshot().stale);
    while (!ack().empty()) {}
    service.process(true); send(packet); service.process(true);
    assert(snapshot().received && snapshot().stats.seq == 1 && snapshot().accepted == 8);
    assert(!ack().empty()); // Old sequence is valid after Wi-Fi restarts.
    service.deinitialize(); service.deinitialize();
    assert(!snapshot().listening && snapshot().accepted == 0);
    // Port is released before the Wi-Fi driver is torn down.
    const int occupied = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); assert(occupied >= 0);
    target.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(bind(occupied, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);
    service.process(true);
    assert(!snapshot().listening && snapshot().error != 0);
    close(occupied);
    fake::now += 999'000; service.process(true); assert(!snapshot().listening);
    fake::now += 1000; service.process(true); assert(snapshot().listening && !snapshot().error);
    service.deinitialize(); close(udp);
}
