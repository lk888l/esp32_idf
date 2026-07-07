#pragma once

#include <array>
#include <atomic>
#include <cstddef>

template<typename T, std::size_t Capacity>
class atomic_array
{
    static_assert(Capacity >= 2);

public:

    bool push(const T& item)
    {
        auto head = head_.load(std::memory_order_relaxed);

        auto next =
            (head + 1) % Capacity;

        auto tail =
            tail_.load(std::memory_order_acquire);

        if(next == tail)
        {
            return false;
        }

        buffer_[head] = item;

        head_.store(
            next,
            std::memory_order_release);

        return true;
    }

    bool pop(T& item)
    {
        auto tail =
            tail_.load(std::memory_order_relaxed);

        auto head =
            head_.load(std::memory_order_acquire);

        if(tail == head)
        {
            return false;
        }

        item = buffer_[tail];

        tail_.store(
            (tail + 1) % Capacity,
            std::memory_order_release);

        return true;
    }

    bool empty() const
    {
        return head_.load() ==
               tail_.load();
    }

    bool full() const
    {
        auto head = head_.load();
        auto next = (head + 1) % Capacity;

        return next ==
               tail_.load();
    }

    std::size_t size() const
    {
        auto head = head_.load();
        auto tail = tail_.load();

        if(head >= tail)
        {
            return head - tail;
        }

        return Capacity - tail + head;
    }

private:

    alignas(32)
    std::array<T, Capacity> buffer_;

    alignas(32)
    std::atomic<size_t> head_{0};

    alignas(32)
    std::atomic<size_t> tail_{0};
};