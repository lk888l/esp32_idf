#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
namespace connectivity::web
{
class ReplayWindow
{
  public:
    enum class Check
    {
        fresh,
        replay,
        expired,
        conflict
    };
    Check check(uint32_t seq, const char *request, const char *&response) const
    {
        if (!seq)
            return Check::expired;
        for (const auto &entry : entries_)
            if (entry.seq == seq)
            {
                if (std::strcmp(entry.request.data(), request))
                    return Check::conflict;
                response = entry.response.data();
                return Check::replay;
            }
        return seq <= highest_ ? Check::expired : Check::fresh;
    }
    void save(uint32_t seq, const char *request, const char *response)
    {
        auto &entry = entries_[seq % entries_.size()];
        entry.seq = seq;
        const size_t request_size = std::min(std::strlen(request), entry.request.size() - 1);
        std::memcpy(entry.request.data(), request, request_size);
        entry.request[request_size] = 0;
        const size_t response_size = std::min(std::strlen(response), entry.response.size() - 1);
        std::memcpy(entry.response.data(), response, response_size);
        entry.response[response_size] = 0;
        highest_ = seq;
    }

  private:
    struct Entry
    {
        uint32_t seq = 0;
        std::array<char, 257> request{};
        std::array<char, 768> response{};
    };
    std::array<Entry, 32> entries_{};
    uint32_t highest_ = 0;
};
template <class T, size_t N> class DropQueue
{
  public:
    bool push(const T &value, T *discarded = nullptr)
    {
        bool dropped = count_ == N;
        if (dropped)
        {
            if (discarded)
                *discarded = data_[first_];
            first_ = (first_ + 1) % N;
            --count_;
        }
        data_[(first_ + count_++) % N] = value;
        return dropped;
    }
    bool pop(T &value)
    {
        if (!count_)
            return false;
        value = data_[first_];
        first_ = (first_ + 1) % N;
        --count_;
        return true;
    }
    void clear()
    {
        first_ = count_ = 0;
    }

  private:
    std::array<T, N> data_{};
    size_t first_ = 0, count_ = 0;
};
} // namespace connectivity::web
