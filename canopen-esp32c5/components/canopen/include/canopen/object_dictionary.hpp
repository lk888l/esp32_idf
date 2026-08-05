#pragma once

#include "canopen/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace canopen {

struct OdKey {
    uint16_t index = 0;
    uint8_t subindex = 0;

    friend constexpr bool operator==(OdKey, OdKey) = default;
};

enum class DataType : uint8_t {
    boolean = 0x01,
    integer8 = 0x02,
    integer16 = 0x03,
    integer32 = 0x04,
    unsigned8 = 0x05,
    unsigned16 = 0x06,
    unsigned32 = 0x07,
    real32 = 0x08,
    visible_string = 0x09,
    octet_string = 0x0A,
    unsigned64 = 0x1B,
};

enum class Access : uint8_t {
    read_only,
    write_only,
    read_write,
};

[[nodiscard]] constexpr bool is_readable(Access access)
{
    return access != Access::write_only;
}

[[nodiscard]] constexpr bool is_writable(Access access)
{
    return access != Access::read_only;
}

struct Entry;
using ReadHook = AbortCode (*)(const Entry&, std::span<uint8_t>, void*);
using WriteHook = AbortCode (*)(const Entry&, std::span<const uint8_t>, void*);

struct Entry {
    OdKey key{};
    DataType type = DataType::octet_string;
    Access access = Access::read_only;
    bool pdo_mappable = false;
    std::span<uint8_t> storage{};
    ReadHook read_hook = nullptr;
    WriteHook write_hook = nullptr;
    void* hook_context = nullptr;
};

class ObjectDictionary {
public:
    static constexpr std::size_t kMaxEntries = 224;

    AbortCode add(Entry entry);

    template <typename T>
    AbortCode add_scalar(OdKey key,
                         T& value,
                         Access access,
                         bool pdo_mappable = false,
                         WriteHook write_hook = nullptr,
                         void* hook_context = nullptr,
                         ReadHook read_hook = nullptr)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return add({
            .key = key,
            .type = type_for<T>(),
            .access = access,
            .pdo_mappable = pdo_mappable,
            .storage = {reinterpret_cast<uint8_t*>(&value), sizeof(T)},
            .read_hook = read_hook,
            .write_hook = write_hook,
            .hook_context = hook_context,
        });
    }

    AbortCode add_bytes(OdKey key,
                        DataType type,
                        std::span<uint8_t> storage,
                        Access access,
                        bool pdo_mappable = false,
                        WriteHook write_hook = nullptr,
                        void* hook_context = nullptr,
                        ReadHook read_hook = nullptr);

    void freeze() { frozen_ = true; }
    [[nodiscard]] bool frozen() const { return frozen_; }
    [[nodiscard]] std::size_t size() const { return size_; }

    [[nodiscard]] Entry* find(OdKey key);
    [[nodiscard]] const Entry* find(OdKey key) const;

    AbortCode read(OdKey key, std::span<uint8_t> destination, std::size_t& bytes_read) const;
    AbortCode write(OdKey key, std::span<const uint8_t> source);

private:
    template <typename T>
    static consteval DataType type_for()
    {
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, uint8_t>) {
            return std::is_same_v<T, bool> ? DataType::boolean : DataType::unsigned8;
        } else if constexpr (std::is_same_v<T, int8_t>) {
            return DataType::integer8;
        } else if constexpr (std::is_same_v<T, uint16_t>) {
            return DataType::unsigned16;
        } else if constexpr (std::is_same_v<T, int16_t>) {
            return DataType::integer16;
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            return DataType::unsigned32;
        } else if constexpr (std::is_same_v<T, int32_t>) {
            return DataType::integer32;
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            return DataType::unsigned64;
        } else if constexpr (std::is_same_v<T, float>) {
            return DataType::real32;
        } else {
            static_assert(!sizeof(T), "unsupported CANopen OD scalar type");
        }
    }

    std::array<Entry, kMaxEntries> entries_{};
    std::size_t size_ = 0;
    bool frozen_ = false;
};

} // namespace canopen
