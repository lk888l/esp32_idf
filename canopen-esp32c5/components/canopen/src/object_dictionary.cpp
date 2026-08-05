#include "canopen/object_dictionary.hpp"

#include <algorithm>
#include <bit>

namespace canopen {
namespace {

bool is_scalar(DataType type)
{
    return type != DataType::visible_string && type != DataType::octet_string;
}

void native_to_little(std::span<const uint8_t> source, std::span<uint8_t> destination)
{
    if constexpr (std::endian::native == std::endian::little) {
        std::copy(source.begin(), source.end(), destination.begin());
    } else {
        std::reverse_copy(source.begin(), source.end(), destination.begin());
    }
}

void little_to_native(std::span<const uint8_t> source, std::span<uint8_t> destination)
{
    if constexpr (std::endian::native == std::endian::little) {
        std::copy(source.begin(), source.end(), destination.begin());
    } else {
        std::reverse_copy(source.begin(), source.end(), destination.begin());
    }
}

} // namespace

AbortCode ObjectDictionary::add(Entry entry)
{
    if (frozen_ || entry.storage.empty() || size_ >= entries_.size()) {
        return AbortCode::out_of_memory;
    }
    if (find(entry.key) != nullptr) {
        return AbortCode::parameter_incompatible;
    }
    entries_[size_++] = entry;
    return AbortCode::none;
}

AbortCode ObjectDictionary::add_bytes(OdKey key,
                                      DataType type,
                                      std::span<uint8_t> storage,
                                      Access access,
                                      bool pdo_mappable,
                                      WriteHook write_hook,
                                      void* hook_context,
                                      ReadHook read_hook)
{
    return add({key, type, access, pdo_mappable, storage, read_hook, write_hook, hook_context});
}

Entry* ObjectDictionary::find(OdKey key)
{
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].key == key) {
            return &entries_[index];
        }
    }
    return nullptr;
}

const Entry* ObjectDictionary::find(OdKey key) const
{
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].key == key) {
            return &entries_[index];
        }
    }
    return nullptr;
}

AbortCode ObjectDictionary::read(OdKey key,
                                 std::span<uint8_t> destination,
                                 std::size_t& bytes_read) const
{
    bytes_read = 0;
    const Entry* entry = find(key);
    if (entry == nullptr) {
        return AbortCode::object_not_found;
    }
    if (!is_readable(entry->access)) {
        return AbortCode::write_only;
    }
    if (destination.size() < entry->storage.size()) {
        return AbortCode::out_of_memory;
    }
    auto output = destination.first(entry->storage.size());
    if (entry->read_hook != nullptr) {
        const AbortCode result = entry->read_hook(*entry, output, entry->hook_context);
        if (result != AbortCode::none) {
            return result;
        }
    } else if (is_scalar(entry->type)) {
        native_to_little(entry->storage, output);
    } else {
        std::copy(entry->storage.begin(), entry->storage.end(), output.begin());
    }
    bytes_read = entry->storage.size();
    return AbortCode::none;
}

AbortCode ObjectDictionary::write(OdKey key, std::span<const uint8_t> source)
{
    Entry* entry = find(key);
    if (entry == nullptr) {
        return AbortCode::object_not_found;
    }
    if (!is_writable(entry->access)) {
        return AbortCode::read_only;
    }
    if (source.size() > entry->storage.size()) {
        return AbortCode::data_too_long;
    }
    if (source.size() < entry->storage.size()) {
        return AbortCode::data_too_short;
    }
    if (entry->write_hook != nullptr) {
        const AbortCode result = entry->write_hook(*entry, source, entry->hook_context);
        if (result != AbortCode::none) {
            return result;
        }
    }
    if (is_scalar(entry->type)) {
        little_to_native(source, entry->storage);
    } else {
        std::copy(source.begin(), source.end(), entry->storage.begin());
    }
    return AbortCode::none;
}

} // namespace canopen

