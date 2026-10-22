// UDP v1 compatibility: espc3-integrated ad1ebef and wl1-studio b969bf7.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "pc_monitor_protocol.hpp"

using namespace pc_monitor;

namespace
{
const std::string packet = R"({"v":1,"session":"a_bc-123","seq":17,"host":"workstation","cpu_pct":25.5,"ram_used":8589934592,"ram_total":17179869184,"gpu_name":"NVIDIA RTX 5070 Ti","gpu_pct":50,"vram_used":4294967296,"vram_total":12884901888,"gpu_temp_c":63.25})";

std::string replace(std::string text, const std::string& before, const std::string& after)
{
    const auto at = text.find(before);
    assert(at != std::string::npos);
    text.replace(at, before.size(), after);
    return text;
}
void rejected(const std::string& input)
{
    Stats stats;
    stats.seq = 42;
    std::strcpy(stats.host, "unchanged");
    assert(!parse_sample(input, stats));
    assert(stats.seq == 42 && !std::strcmp(stats.host, "unchanged"));
}
void parsing()
{
    Stats stats;
    assert(parse_sample(packet, stats));
    assert(stats.seq == 17 && !std::strcmp(stats.session, "a_bc-123"));
    assert(!std::strcmp(stats.host, "workstation"));
    assert(stats.cpu_valid && stats.cpu_pct == 25.5f);
    assert(stats.gpu_valid && stats.gpu_pct == 50);
    assert(stats.gpu_temp_valid && stats.gpu_temp_c == 63.25f);
    assert(stats.ram_used == 8589934592ULL && stats.ram_total == 17179869184ULL);
    assert(stats.vram_valid && stats.vram_used == 4294967296ULL);
    assert(parse_sample(" \r\n\t" + packet + "  ", stats));

    auto missing_gpu = replace(packet, "\"cpu_pct\":25.5", "\"cpu_pct\":null");
    missing_gpu = replace(missing_gpu, "\"gpu_pct\":50", "\"gpu_pct\":null");
    missing_gpu = replace(missing_gpu, "\"gpu_temp_c\":63.25", "\"gpu_temp_c\":null");
    missing_gpu = replace(missing_gpu, "\"vram_used\":4294967296", "\"vram_used\":null");
    missing_gpu = replace(missing_gpu, "\"vram_total\":12884901888", "\"vram_total\":null");
    missing_gpu = replace(missing_gpu, "NVIDIA RTX 5070 Ti", "");
    assert(parse_sample(missing_gpu, stats));
    assert(!stats.cpu_valid && !stats.gpu_valid && !stats.gpu_temp_valid && !stats.vram_valid);

    auto unicode = replace(packet, "workstation", R"(host-\u4e2d\u6587-\ud83d\ude3a)");
    assert(parse_sample(unicode, stats));
    assert(std::string(stats.host) == "host-中文-😺");
    assert(parse_sample(replace(packet, "workstation", "中文"), stats));
    assert(parse_sample(replace(packet, "NVIDIA RTX 5070 Ti", R"(NVIDIA \"RTX\" \\ gpu)"), stats));
    assert(parse_sample(replace(packet, "25.5", "2.5e1"), stats) && stats.cpu_pct == 25);
    assert(parse_sample(replace(packet, "\"seq\":17", "\"seq\":4294967295"), stats));
    assert(stats.seq == UINT32_MAX);
    assert(parse_sample(replace(packet, "17179869184", "18446744073709551615"), stats));
    assert(stats.ram_total == UINT64_MAX);
}
void malformed()
{
    rejected("");
    rejected("{}");
    rejected("null");
    rejected(packet + "x");
    rejected(packet + std::string(max_packet_bytes - packet.size(), ' '));
    rejected(replace(packet, "\"v\":1", "\"v\":2"));
    rejected(replace(packet, "\"v\":1", "\"v\":1,\"v\":1"));
    rejected(replace(packet, "\"v\":1,", ""));
    rejected(replace(packet, "\"v\":1", "\"extra\":1"));
    rejected(replace(packet, "\"v\":1", "\"v\":true"));
    rejected(replace(packet, "\"seq\":17", "\"seq\":4294967296"));
    rejected(replace(packet, "\"seq\":17", "\"seq\":-1"));
    rejected(replace(packet, "\"seq\":17", "\"seq\":1.0"));
    rejected(replace(packet, "\"seq\":17", "\"seq\":01"));
    rejected(replace(packet, "25.5", "NaN"));
    rejected(replace(packet, "25.5", "Infinity"));
    rejected(replace(packet, "25.5", "1e999"));
    rejected(replace(packet, "25.5", "+1"));
    rejected(replace(packet, "25.5", ".1"));
    rejected(replace(packet, "25.5", "1."));
    rejected(replace(packet, "25.5", "1e"));
    rejected(replace(packet, "25.5", "101"));
    rejected(replace(packet, "25.5", "-0.1"));
    rejected(replace(packet, "\"gpu_pct\":50", "\"gpu_pct\":100.01"));
    rejected(replace(packet, "\"gpu_temp_c\":63.25", "\"gpu_temp_c\":251"));
    rejected(replace(packet, "\"gpu_temp_c\":63.25", "\"gpu_temp_c\":-101"));
    rejected(replace(packet, "17179869184", "0"));
    rejected(replace(packet, "17179869184", "1"));
    rejected(replace(packet, "17179869184", "18446744073709551616"));
    rejected(replace(packet, "17179869184", "1e20"));
    rejected(replace(packet, "\"vram_total\":12884901888", "\"vram_total\":null"));
    rejected(replace(packet, "\"vram_total\":12884901888", "\"vram_total\":1"));
    rejected(replace(packet, "a_bc-123", ""));
    rejected(replace(packet, "a_bc-123", "unsafe\\\"session"));
    rejected(replace(packet, "a_bc-123", std::string(33, 'a')));
    rejected(replace(packet, "workstation", ""));
    rejected(replace(packet, "workstation", std::string(65, 'a')));
    rejected(replace(packet, "workstation", R"(bad\nline)"));
    rejected(replace(packet, "workstation", R"(bad\u0000line)"));
    rejected(replace(packet, "workstation", R"(bad\ud800)"));
    rejected(replace(packet, "workstation", R"(bad\udfff)"));
    rejected(replace(packet, "workstation", R"(bad\x41)"));
    rejected(replace(packet, "workstation", std::string("bad") + char(0xc0) + char(0xaf)));
    rejected(replace(packet, "workstation", std::string("bad") + char(0xed) + char(0xa0) + char(0x80)));
    rejected(replace(packet, "NVIDIA RTX 5070 Ti", std::string(97, 'a')));
    // Every truncated prefix is rejected without overwriting the last good state.
    for (std::size_t i = 0; i < packet.size(); ++i) rejected(packet.substr(0, i));
}
void freshness()
{
    Stats stats;
    assert(parse_sample(packet, stats));
    LatestSample latest;
    assert(!latest.received() && latest.stale(0));
    assert(latest.accept(stats, 100));
    assert(latest.received() && !latest.stale(3099) && latest.stale(3100));
    assert(latest.age_ms(500) == 400);
    assert(!latest.accept(stats, 1000));
    assert(latest.age_ms(1100) == 1000);  // duplicate cannot refresh expiry
    --stats.seq;
    assert(!latest.accept(stats, 1000));
    stats.seq += 2;
    assert(latest.accept(stats, 1000) && latest.stats().seq == 18);
    std::strcpy(stats.session, "new-session");
    stats.seq = 0;
    assert(latest.accept(stats, 2000));
    latest.reset();
    assert(!latest.received() && latest.age_ms(3000) == 0);
    stats.seq = UINT32_MAX;
    assert(latest.accept(stats, UINT32_MAX - 1000));
    stats.seq = 0;
    assert(latest.accept(stats, UINT32_MAX - 500));
    assert(latest.age_ms(500) == 1001 && !latest.stale(500));
    assert(latest.stale(2499));
    assert(!newer_sequence(1, 1) && newer_sequence(0, UINT32_MAX));
    assert(!newer_sequence(UINT32_MAX, 0));
    assert(!newer_sequence(0x80000000, 0));
}
}  // namespace

int main()
{
    parsing();
    malformed();
    freshness();
    std::cout << "PC monitor parsing, bounds, ordering and freshness passed\n";
}
