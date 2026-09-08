#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

#include "connectivity_policy.hpp"

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "Connectivity policy test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

connectivity::WifiCredentials credentials(std::string_view ssid,
                                           std::string_view password)
{
    connectivity::WifiCredentials result{};
    check(ssid.size() < sizeof(result.ssid), "test SSID fits storage");
    check(password.size() < sizeof(result.password), "test password fits storage");
    std::copy(ssid.begin(), ssid.end(), result.ssid);
    std::copy(password.begin(), password.end(), result.password);
    return result;
}

void test_credential_boundaries()
{
    using connectivity::valid_credentials;
    check(!valid_credentials({}), "empty SSID is rejected");
    check(!valid_credentials(credentials("", "password")), "password requires SSID");
    check(valid_credentials(credentials("home", "password")), "normal WPA passphrase");
    check(valid_credentials(credentials("a", "")), "explicit open STA network");
    check(!valid_credentials(credentials("a", "1234567")), "seven byte passphrase rejected");
    check(valid_credentials(credentials("a", "12345678")), "eight byte passphrase accepted");

    std::array<char, 63> passphrase{};
    passphrase.fill('!');
    check(valid_credentials(credentials("a", {passphrase.data(), passphrase.size()})),
          "63 byte passphrase accepted");

    std::array<char, 32> full_ssid{};
    full_ssid.fill('s');
    check(valid_credentials(credentials({full_ssid.data(), full_ssid.size()}, "password")),
          "32 byte SSID accepted");

    auto unterminated_ssid = credentials("home", "password");
    std::memset(unterminated_ssid.ssid, 's', sizeof(unterminated_ssid.ssid));
    check(!valid_credentials(unterminated_ssid), "SSID requires in-bounds NUL terminator");

    auto unterminated_password = credentials("home", "password");
    std::memset(unterminated_password.password, 'a', sizeof(unterminated_password.password));
    check(!valid_credentials(unterminated_password), "password requires in-bounds NUL terminator");
}

void test_hex_psk()
{
    constexpr std::string_view hex_psk =
        "0123456789abcdefABCDEF01234567890123456789abcdefABCDEF0123456789";
    static_assert(hex_psk.size() == 64);
    auto key = credentials("home", hex_psk);
    check(connectivity::valid_credentials(key), "mixed case 64 digit hex PSK accepted");
    key.password[0] = 'g';
    check(!connectivity::valid_credentials(key), "nonhex PSK first digit rejected");
    key.password[0] = '0';
    key.password[63] = ' ';
    check(!connectivity::valid_credentials(key), "nonhex PSK last digit rejected");
    key.password[63] = static_cast<char>(0xFF);
    check(!connectivity::valid_credentials(key), "non-ASCII hex digit rejected");
}

void test_utf8_boundaries()
{
    using connectivity::valid_utf8;
    check(valid_utf8("ASCII"), "ASCII valid");
    check(valid_utf8("\xE4\xB8\xAD\xF0\x9F\x98\x80"), "multibyte UTF8 valid");
    check(!valid_utf8("\xC0\xAF"), "overlong UTF8 rejected");
    check(!valid_utf8("\xED\xA0\x80"), "surrogate rejected");
    check(!valid_utf8("\xF4\x90\x80\x80"), "codepoint overflow rejected");
    check(!valid_utf8("\xE4\xB8"), "truncated UTF8 rejected");
    char output[4]{};
    connectivity::copy_display_utf8("\xE4\xB8\xAD!", output, sizeof(output));
    check(std::string_view(output) == "\xE4\xB8\xAD", "display truncates at character boundary");
    connectivity::copy_display_utf8("\xFF!", output, sizeof(output));
    check(std::string_view(output) == "?!", "invalid display byte replaced");
}

void test_ble_address_validation()
{
    using connectivity::valid_ble_address;
    check(valid_ble_address("01:23:45:67:89:AB"), "public or random address shape");
    check(valid_ble_address("aa:bb:cc:dd:ee:ff"), "lowercase address accepted");
    check(!valid_ble_address(""), "empty address rejected");
    check(!valid_ble_address("01:23:45:67:89"), "short address rejected");
    check(!valid_ble_address("01-23-45-67-89-AB"), "wrong separator rejected");
    check(!valid_ble_address("01:23:45:67:89:AZ"), "nonhex address rejected");
    check(!valid_ble_address("01:23:45:67:89:AB "), "trailing data rejected");
}

void test_reconnect_backoff()
{
    connectivity::RetryBackoff backoff;
    constexpr std::array<uint32_t, 7> expected{
        1'000, 2'000, 4'000, 8'000, 16'000, 32'000, 60'000,
    };
    for (const auto delay : expected) {
        check(backoff.next_delay_ms() == delay, "exponential reconnect delay");
    }
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        check(backoff.next_delay_ms() == 60'000, "reconnect delay saturates without overflow");
    }
    backoff.reset();
    check(backoff.next_delay_ms() == 1'000, "reset restores first delay");
    check(backoff.next_delay_ms() == 2'000, "backoff resumes after reset");
}

void test_token_comparison()
{
    using connectivity::constant_time_equal;
    check(constant_time_equal("same-token", "same-token"), "equal tokens");
    check(!constant_time_equal("same-token", "Same-token"), "first byte mismatch");
    check(!constant_time_equal("same-token", "same_Token"), "middle byte mismatch");
    check(!constant_time_equal("same-token", "same-tokeN"), "last byte mismatch");
    check(!constant_time_equal("same-token", "same-token-extra"), "longer candidate");
    check(!constant_time_equal("same-token-extra", "same-token"), "shorter candidate");
    check(constant_time_equal({}, {}), "empty values compare equally as raw strings");
    check(!constant_time_equal({}, "a"), "empty expected value differs");
    check(!constant_time_equal("a", {}), "empty candidate differs");
    constexpr std::array<char, 3> embedded_nul{'a', '\0', 'b'};
    const std::string_view binary{embedded_nul.data(), embedded_nul.size()};
    check(constant_time_equal(binary, binary), "embedded NUL does not truncate comparison");
    check(!constant_time_equal(binary, "a"), "length differs beyond NUL");
    constexpr std::array<char, 2> trailing_nul{'a', '\0'};
    check(!constant_time_equal({trailing_nul.data(), trailing_nul.size()}, "a"),
          "NUL padding cannot hide unequal lengths");
    constexpr std::array<char, 2> high_bytes{static_cast<char>(0x80), static_cast<char>(0xFF)};
    const std::string_view high{high_bytes.data(), high_bytes.size()};
    check(constant_time_equal(high, high), "signed char bytes compare equally");
    check(!constant_time_equal(high, "aa"), "signed char bytes remain distinct");
}

} // namespace

int main()
{
    test_credential_boundaries();
    test_hex_psk();
    test_utf8_boundaries();
    test_ble_address_validation();
    test_reconnect_backoff();
    test_token_comparison();
    std::cout << "All connectivity policy tests passed\n";
    return EXIT_SUCCESS;
}
