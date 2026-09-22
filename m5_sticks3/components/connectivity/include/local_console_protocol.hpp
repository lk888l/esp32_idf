#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "connectivity_policy.hpp"
namespace connectivity::console {
inline constexpr size_t kMaxLineBytes = 512;
inline constexpr char kRecordSeparator = '\x1e';
// Reject a damaged line AND its suffix until the delimiter. CRLF is one frame.
class LineFramer {
public:
    enum class Result { none, line, too_long, invalid_character };
    Result push(char c)
    {
        if (c == '\n' && after_cr_) { after_cr_ = false; return Result::none; }
        after_cr_ = c == '\r';
        if (c == '\r' || c == '\n') {
            const auto result = error_ != Result::none ? error_ : length_ ? Result::line : Result::none;
            buffer_[length_] = 0; length_ = 0; error_ = Result::none;
            return result;
        }
        if (error_ != Result::none) return Result::none;
        if (c == kRecordSeparator && length_ == 0) return Result::none;
        if (c == '\b' || c == '\x7f') {
            if (length_) {
                --length_;
                while (length_ && (static_cast<unsigned char>(buffer_[length_]) & 0xc0) == 0x80) --length_;
            }
            return Result::none;
        }
        if (static_cast<unsigned char>(c) < 32 && c != '\t') error_ = Result::invalid_character;
        else if (length_ == kMaxLineBytes) error_ = Result::too_long;
        else buffer_[length_++] = c;
        return Result::none;
    }
    const char* line() const { return buffer_.data(); }
    void erase_line() { buffer_.fill(0); } // Preserve CRLF state after dispatch.
    bool partial() const { return length_ || error_ != Result::none; }
    void abandon() { length_ = 0; error_ = Result::invalid_character; }
    void reset() { buffer_.fill(0); length_ = 0; error_ = Result::none; after_cr_ = false; }
private:
    std::array<char, kMaxLineBytes + 1> buffer_{};
    size_t length_ = 0;
    Result error_ = Result::none;
    bool after_cr_ = false;
};
// Human commands compile to the same envelope as USB JSON. No heap/history/echo.
// Returns a stable error code, or nullptr on success.
const char* compile(const char* line, uint32_t id, char* json, size_t capacity);
} // namespace connectivity::console
