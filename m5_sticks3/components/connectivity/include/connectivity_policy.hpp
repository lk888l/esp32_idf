#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace connectivity {

inline constexpr std::size_t kMaxRequestBytes = 256;
inline constexpr std::size_t kMaxResponseBytes = 768;

struct WifiCredentials {
    char ssid[33]{};
    char password[65]{};
};

namespace detail {

template <std::size_t Size>
inline std::size_t bounded_length(const char (&text)[Size])
{
    std::size_t length = 0;
    while (length < Size && text[length] != '\0') {
        ++length;
    }
    return length;
}

inline bool is_hex_digit(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

} // namespace detail

// Lengths are byte counts. An empty password explicitly selects an open STA
// network; WPA credentials are an 8..63 byte passphrase or a 64 digit hex PSK.
inline bool valid_credentials(const WifiCredentials& credentials)
{
    const std::size_t ssid_length = detail::bounded_length(credentials.ssid);
    const std::size_t password_length = detail::bounded_length(credentials.password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64) {
        return false;
    }
    if (password_length == 0 || (password_length >= 8 && password_length <= 63)) {
        return true;
    }
    if (password_length != 64) {
        return false;
    }
    for (std::size_t index = 0; index < password_length; ++index) {
        if (!detail::is_hex_digit(credentials.password[index])) {
            return false;
        }
    }
    return true;
}

// Validate a bounded UTF-8 sequence without accepting overlong forms,
// UTF-16 surrogates or code points beyond U+10FFFF.
inline size_t utf8_character_bytes(std::string_view text)
{
    if (text.empty()) return 0;
    const auto at = [&](size_t index) { return static_cast<unsigned char>(text[index]); };
    const unsigned first = at(0);
    if (first < 0x80) return 1;
    const size_t count = first >= 0xC2 && first <= 0xDF ? 2 :
                         first >= 0xE0 && first <= 0xEF ? 3 :
                         first >= 0xF0 && first <= 0xF4 ? 4 : 0;
    if (!count || text.size() < count) return 0;
    for (size_t i = 1; i < count; ++i) if ((at(i) & 0xC0) != 0x80) return 0;
    if ((first == 0xE0 && at(1) < 0xA0) || (first == 0xED && at(1) >= 0xA0) ||
        (first == 0xF0 && at(1) < 0x90) || (first == 0xF4 && at(1) >= 0x90)) return 0;
    return count;
}
inline bool valid_utf8(std::string_view text)
{
    while (!text.empty()) {
        const size_t count = utf8_character_bytes(text);
        if (!count) return false;
        text.remove_prefix(count);
    }
    return true;
}
inline void copy_display_utf8(std::string_view text, char* output, size_t capacity)
{
    if (!capacity) return;
    size_t written = 0;
    while (!text.empty()) {
        const size_t count = utf8_character_bytes(text);
        const size_t consumed = count ? count : 1;
        if (written + consumed >= capacity) break;
        if (count) {
            for (size_t i = 0; i < count; ++i) output[written++] = text[i];
        } else {
            output[written++] = '?';
        }
        text.remove_prefix(consumed);
    }
    output[written] = '\0';
}

inline bool valid_ble_address(std::string_view address)
{
    if (address.size() != 17) return false;
    for (size_t i = 0; i < address.size(); ++i) {
        if (i % 3 == 2) {
            if (address[i] != ':') return false;
        } else if (!detail::is_hex_digit(address[i])) return false;
    }
    return true;
}

// Call once per failed connection attempt; reset only after acquiring an IP
// address or when replacing credentials, so repeated failures keep backing off.
class RetryBackoff {
public:
    uint32_t next_delay_ms()
    {
        const uint32_t delay = next_delay_ms_;
        next_delay_ms_ = delay < 30'000 ? delay * 2 : 60'000;
        return delay;
    }

    void reset() { next_delay_ms_ = 1'000; }

private:
    uint32_t next_delay_ms_ = 1'000;
};

// Buffer lengths are public. Every byte is examined, including when lengths
// differ; volatile prevents the compiler replacing the loop with an early-exit
// comparison. Authentication callers must separately reject an empty token.
inline bool constant_time_equal(std::string_view a, std::string_view b)
{
    volatile std::size_t difference = a.size() ^ b.size();
    const std::size_t length = a.size() > b.size() ? a.size() : b.size();
    for (std::size_t index = 0; index < length; ++index) {
        const auto left = index < a.size() ? static_cast<unsigned char>(a[index]) : 0U;
        const auto right = index < b.size() ? static_cast<unsigned char>(b[index]) : 0U;
        difference = difference | (left ^ right);
    }
    return difference == 0;
}

} // namespace connectivity
