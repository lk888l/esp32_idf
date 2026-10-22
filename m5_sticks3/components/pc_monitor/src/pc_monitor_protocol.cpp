// UDP v1 compatibility: espc3-integrated ad1ebef and wl1-studio b969bf7.
#include "pc_monitor_protocol.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace pc_monitor
{
namespace
{
class Json
{
   public:
    explicit Json(std::string_view input) : input_(input) {}
    void space()
    {
        while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\r' ||
               input_[at_] == '\n' || input_[at_] == '\t')) ++at_;
    }
    bool take(char expected)
    {
        space();
        if (at_ == input_.size() || input_[at_] != expected) return false;
        ++at_;
        return true;
    }
    bool done() { space(); return at_ == input_.size(); }
    bool null()
    {
        space();
        if (input_.substr(at_, 4) != "null") return false;
        at_ += 4;
        return true;
    }
    bool number(std::string_view& token)
    {
        space();
        const auto begin = at_;
        if (at_ < input_.size() && input_[at_] == '-') ++at_;
        if (at_ == input_.size()) return false;
        if (input_[at_] == '0') ++at_;
        else
        {
            if (input_[at_] < '1' || input_[at_] > '9') return false;
            while (digit()) ++at_;
        }
        if (at_ < input_.size() && input_[at_] == '.')
        {
            ++at_;
            if (!digit()) return false;
            while (digit()) ++at_;
        }
        if (at_ < input_.size() && (input_[at_] == 'e' || input_[at_] == 'E'))
        {
            ++at_;
            if (at_ < input_.size() && (input_[at_] == '-' || input_[at_] == '+')) ++at_;
            if (!digit()) return false;
            while (digit()) ++at_;
        }
        token = input_.substr(begin, at_ - begin);
        return true;
    }
    bool string(char* output, std::size_t capacity)
    {
        if (!take('"')) return false;
        std::size_t used = 0;
        const auto append = [&](uint8_t byte)
        {
            if (used + 1 >= capacity || byte < 0x20 || byte == 0x7f) return false;
            output[used++] = char(byte);
            return true;
        };
        while (at_ < input_.size())
        {
            uint32_t byte = uint8_t(input_[at_++]);
            if (byte == '"') { output[used] = 0; return true; }
            if (byte == '\\')
            {
                if (at_ == input_.size()) return false;
                byte = uint8_t(input_[at_++]);
                if (byte == '"' || byte == '\\' || byte == '/')
                {
                    if (!append(uint8_t(byte))) return false;
                    continue;
                }
                if (byte != 'u' || !hex4(byte)) return false;
                if (byte >= 0xd800 && byte <= 0xdbff)
                {
                    uint32_t low;
                    if (at_ + 2 > input_.size() || input_[at_++] != '\\' ||
                        input_[at_++] != 'u' || !hex4(low) || low < 0xdc00 || low > 0xdfff)
                        return false;
                    byte = 0x10000 + ((byte - 0xd800) << 10) + low - 0xdc00;
                }
                else if (byte >= 0xdc00 && byte <= 0xdfff) return false;
                if (byte < 0x80) { if (!append(uint8_t(byte))) return false; }
                else if (byte < 0x800)
                {
                    if (!append(uint8_t(0xc0 | (byte >> 6))) ||
                        !append(uint8_t(0x80 | (byte & 63)))) return false;
                }
                else if (byte < 0x10000)
                {
                    if (!append(uint8_t(0xe0 | (byte >> 12))) ||
                        !append(uint8_t(0x80 | ((byte >> 6) & 63))) ||
                        !append(uint8_t(0x80 | (byte & 63)))) return false;
                }
                else
                {
                    if (!append(uint8_t(0xf0 | (byte >> 18))) ||
                        !append(uint8_t(0x80 | ((byte >> 12) & 63))) ||
                        !append(uint8_t(0x80 | ((byte >> 6) & 63))) ||
                        !append(uint8_t(0x80 | (byte & 63)))) return false;
                }
                continue;
            }
            if (byte < 0x80) { if (!append(uint8_t(byte))) return false; continue; }
            // Validate raw UTF-8 as well as escaped Unicode.
            unsigned extra;
            uint32_t codepoint;
            if (byte >= 0xc2 && byte <= 0xdf) { extra = 1; codepoint = byte & 31; }
            else if (byte >= 0xe0 && byte <= 0xef) { extra = 2; codepoint = byte & 15; }
            else if (byte >= 0xf0 && byte <= 0xf4) { extra = 3; codepoint = byte & 7; }
            else return false;
            if (!append(uint8_t(byte))) return false;
            for (unsigned i = 0; i < extra; ++i)
            {
                if (at_ == input_.size()) return false;
                const auto continuation = uint8_t(input_[at_++]);
                if ((continuation & 0xc0) != 0x80 || !append(continuation)) return false;
                codepoint = (codepoint << 6) | (continuation & 63);
            }
            if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
                (extra == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        }
        return false;
    }

   private:
    std::string_view input_;
    std::size_t at_ = 0;
    bool digit() const { return at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9'; }
    bool hex4(uint32_t& result)
    {
        result = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (at_ == input_.size()) return false;
            const char c = input_[at_++];
            const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (value < 0) return false;
            result = (result << 4) | unsigned(value);
        }
        return true;
    }
};

bool integer(Json& json, uint64_t& output)
{
    std::string_view token;
    if (!json.number(token) || token.empty()) return false;
    for (const auto c : token) if (c < '0' || c > '9') return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}
bool optional_float(Json& json, float& output, bool& valid, double minimum, double maximum)
{
    if (json.null()) { valid = false; output = 0; return true; }
    std::string_view token;
    if (!json.number(token) || token.size() >= 64) return false;
    char buffer[64]{};
    std::memcpy(buffer, token.data(), token.size());
    char* end = nullptr;
    const double value = std::strtod(buffer, &end);
    if (end != buffer + token.size() || !std::isfinite(value) || value < minimum || value > maximum)
        return false;
    output = float(value);
    valid = true;
    return true;
}
}  // namespace

bool parse_sample(std::string_view packet, Stats& output)
{
    if (packet.empty() || packet.size() >= max_packet_bytes) return false;
    static constexpr std::string_view keys[] = {"v", "session", "seq", "host", "cpu_pct", "ram_used",
        "ram_total", "gpu_name", "gpu_pct", "vram_used", "vram_total", "gpu_temp_c"};
    Json json(packet);
    Stats next;
    uint16_t seen = 0;
    bool vram_used_valid = false, vram_total_valid = false;
    if (!json.take('{')) return false;
    for (unsigned field = 0; field < 12; ++field)
    {
        char key[24];
        if (!json.string(key, sizeof(key)) || !json.take(':')) return false;
        unsigned index = 0;
        while (index < 12 && keys[index] != key) ++index;
        if (index == 12 || (seen & (1U << index))) return false;
        seen |= uint16_t(1U << index);
        uint64_t value = 0;
        switch (index)
        {
            case 0: if (!integer(json, value) || value != 1) return false; break;
            case 1:
                if (!json.string(next.session, sizeof(next.session)) || !next.session[0]) return false;
                for (const char c : std::string_view(next.session))
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
                break;
            case 2:
                if (!integer(json, value) || value > UINT32_MAX) return false;
                next.seq = uint32_t(value); break;
            case 3: if (!json.string(next.host, sizeof(next.host)) || !next.host[0]) return false; break;
            case 4: if (!optional_float(json, next.cpu_pct, next.cpu_valid, 0, 100)) return false; break;
            case 5: if (!integer(json, next.ram_used)) return false; break;
            case 6: if (!integer(json, next.ram_total)) return false; break;
            case 7: if (!json.string(next.gpu_name, sizeof(next.gpu_name))) return false; break;
            case 8: if (!optional_float(json, next.gpu_pct, next.gpu_valid, 0, 100)) return false; break;
            case 9:
                if (!json.null()) { if (!integer(json, next.vram_used)) return false; vram_used_valid = true; }
                break;
            case 10:
                if (!json.null()) { if (!integer(json, next.vram_total)) return false; vram_total_valid = true; }
                break;
            case 11:
                if (!optional_float(json, next.gpu_temp_c, next.gpu_temp_valid, -100, 250)) return false;
                break;
        }
        if (field < 11 && !json.take(',')) return false;
    }
    if (!json.take('}') || !json.done() || !next.ram_total || next.ram_used > next.ram_total ||
        vram_used_valid != vram_total_valid || (vram_total_valid &&
        (!next.vram_total || next.vram_used > next.vram_total))) return false;
    next.vram_valid = vram_total_valid;
    output = next;
    return true;
}

bool newer_sequence(uint32_t incoming, uint32_t previous)
{
    const uint32_t difference = incoming - previous;
    return difference && difference < 0x80000000U;
}
bool LatestSample::accept(const Stats& incoming, uint32_t received_ms)
{
    if (received_ && !std::strcmp(incoming.session, stats_.session) &&
        !newer_sequence(incoming.seq, stats_.seq)) return false;
    stats_ = incoming;
    received_ms_ = received_ms;
    received_ = true;
    return true;
}
void LatestSample::reset() { stats_ = {}; received_ms_ = 0; received_ = false; }
uint32_t LatestSample::age_ms(uint32_t now) const { return received_ ? now - received_ms_ : 0; }
bool LatestSample::stale(uint32_t now) const { return !received_ || age_ms(now) >= stale_after_ms; }
}  // namespace pc_monitor
