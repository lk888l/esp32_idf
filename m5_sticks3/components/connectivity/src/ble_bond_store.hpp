#pragma once

#include <cstddef>
#include <cstring>

#include "host/ble_hs.h"
#include "host/ble_store.h"

namespace connectivity::detail {

// IDF 5.5.4's config store persists additions/deletions but skips same-count
// security updates. Decorate its public callbacks, under the caller's host
// lock, without depending on its private RAM arrays or NVS key layout.
class BleBondStore final {
public:
    using Report = void (*)(int object, int result, int rollback, bool update);

    BleBondStore(ble_store_read_fn* read, ble_store_write_fn* write,
                 ble_store_delete_fn* erase, Report report)
        : read_(read), write_(write), erase_(erase), report_(report) {}

    void reset() { error_ = 0; }
    int error() const { return error_; }

    int write(int object, const ble_store_value* incoming)
    {
        if (object != BLE_STORE_OBJ_TYPE_OUR_SEC &&
            object != BLE_STORE_OBJ_TYPE_PEER_SEC) {
            return write_(object, incoming);
        }
        if (error_ != 0) return error_;
        if (incoming == nullptr) return fail(object, BLE_HS_EINVAL, 0, false);

        ble_store_key key{};
        ble_store_key_from_value_sec(&key.sec, &incoming->sec);
        SavedRecord previous;
        const int found = read_(object, &key, &previous.value);
        if (found == BLE_HS_ENOENT) {
            const int result = write_(object, incoming);
            return result == 0 ? 0 : fail(object, result, 0, false);
        }
        if (found != 0) return fail(object, found, 0, false);
        if (same_security(previous.value.sec, incoming->sec)) return 0;

        int result = erase_(object, &key);
        if (result == 0) result = write_(object, incoming);
        if (result != 0) {
            // The default backend changes RAM before persistence, so even a
            // failed operation may have changed the record. Restore through
            // the same callbacks; never report the failed update as success.
            const int rollback = restore(object, key, previous.value);
            return fail(object, result, rollback, true);
        }
        if (report_ != nullptr) report_(object, 0, 0, true);
        return 0;
    }

private:
    struct SavedRecord {
        ble_store_value value{};
        ~SavedRecord()
        {
            // Keep temporary key material out of reused stack storage.
            volatile unsigned char* bytes =
                reinterpret_cast<volatile unsigned char*>(&value);
            for (size_t index = 0; index < sizeof(value); ++index) bytes[index] = 0;
        }
    };

    static bool same_security(const ble_store_value_sec& left,
                              const ble_store_value_sec& right)
    {
        // bond_count is backend bookkeeping; structure padding and absent
        // key bytes are not security values and must not trigger flash wear.
        return ble_addr_cmp(&left.peer_addr, &right.peer_addr) == 0 &&
            left.ltk_present == right.ltk_present &&
            left.irk_present == right.irk_present &&
            left.csrk_present == right.csrk_present &&
            left.authenticated == right.authenticated && left.sc == right.sc &&
            (!left.ltk_present ||
             (left.key_size == right.key_size && left.ediv == right.ediv &&
              left.rand_num == right.rand_num &&
              std::memcmp(left.ltk, right.ltk, sizeof(left.ltk)) == 0)) &&
            (!left.irk_present ||
             std::memcmp(left.irk, right.irk, sizeof(left.irk)) == 0) &&
            (!left.csrk_present ||
             (left.sign_counter == right.sign_counter &&
              std::memcmp(left.csrk, right.csrk, sizeof(left.csrk)) == 0));
    }

    int restore(int object, const ble_store_key& key,
                const ble_store_value& previous)
    {
        SavedRecord current;
        const int found = read_(object, &key, &current.value);
        if (found == 0 && same_security(current.value.sec, previous.sec)) return 0;
        if (found != 0 && found != BLE_HS_ENOENT) return found;

        int cleanup = 0;
        if (found == 0) cleanup = erase_(object, &key);
        // A failed delete can still remove the RAM entry. Attempt restoring
        // the old value regardless, and preserve any cleanup error as well.
        const int restored = write_(object, &previous);
        return restored != 0 ? restored : cleanup;
    }

    int fail(int object, int result, int rollback, bool update)
    {
        error_ = result;
        if (report_ != nullptr) report_(object, result, rollback, update);
        return result;
    }

    ble_store_read_fn* read_;
    ble_store_write_fn* write_;
    ble_store_delete_fn* erase_;
    Report report_;
    int error_ = 0; // Only the NimBLE host uses the adapter after start().
};

} // namespace connectivity::detail
