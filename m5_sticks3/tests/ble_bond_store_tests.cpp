#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "ble_bond_store.hpp"

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::exit(1); } } while (false)

namespace {
using connectivity::detail::BleBondStore;
using Records = std::vector<ble_store_value>;
struct Fault { int call; int result; bool after_ram; };
struct Report { int object; int result; int rollback; bool update; };

// Model the installed IDF 5.5.4 backend's public callback contract and its
// count-only NVS persistence. Test records are generated synthetic values.
struct Backend {
    std::array<Records, 4> ram;
    std::array<Records, 4> flash;
    std::vector<Fault> write_faults;
    std::vector<Fault> delete_faults;
    std::vector<Report> reports;
    int read_error = 0;
    int reads = 0, writes = 0, deletes = 0;
    static Backend* active;

    static int index(const Records& records, const ble_addr_t& peer) {
        for (size_t i = 0; i < records.size(); ++i)
            if (ble_addr_cmp(&records[i].sec.peer_addr, &peer) == 0) return static_cast<int>(i);
        return -1;
    }
    static const Fault* fault(const std::vector<Fault>& faults, int call) {
        for (const auto& item : faults) if (item.call == call) return &item;
        return nullptr;
    }
    void persist_count(int object) {
        auto& disk = flash[object];
        const auto& memory = ram[object];
        if (disk.size() < memory.size()) {
            disk.push_back(memory.back());
        } else if (disk.size() > memory.size()) {
            const auto stale = std::find_if(disk.begin(), disk.end(), [&](const auto& item) {
                return index(memory, item.sec.peer_addr) < 0;
            });
            CHECK(stale != disk.end());
            disk.erase(stale);
        }
        // Equal counts deliberately skip updates, matching the SDK defect.
    }
    static int read(int object, const ble_store_key* key, ble_store_value* value) {
        auto& self = *active;
        ++self.reads;
        if (self.read_error != 0) return self.read_error;
        const int found = index(self.ram[object], key->sec.peer_addr);
        if (found < 0) return BLE_HS_ENOENT;
        *value = self.ram[object][found];
        return 0;
    }
    static int write(int object, const ble_store_value* value) {
        auto& self = *active;
        const Fault* injected = fault(self.write_faults, ++self.writes);
        if (injected != nullptr && !injected->after_ram) return injected->result;
        auto& records = self.ram[object];
        const int found = index(records, value->sec.peer_addr);
        if (found < 0) records.push_back(*value);
        else records[found] = *value;
        if (injected != nullptr) return injected->result;
        self.persist_count(object);
        return 0;
    }
    static int erase(int object, const ble_store_key* key) {
        auto& self = *active;
        const Fault* injected = fault(self.delete_faults, ++self.deletes);
        if (injected != nullptr && !injected->after_ram) return injected->result;
        auto& records = self.ram[object];
        const int found = index(records, key->sec.peer_addr);
        if (found < 0) return BLE_HS_ENOENT;
        records.erase(records.begin() + found);
        if (injected != nullptr) return injected->result;
        self.persist_count(object);
        return 0;
    }
    static void report(int object, int result, int rollback, bool update) {
        active->reports.push_back({object, result, rollback, update});
    }
    Backend() { active = this; }
    void seed(int object, const ble_store_value& value) {
        ram[object].push_back(value);
        flash[object].push_back(value);
    }
    void reboot() { ram = flash; }
    BleBondStore adapter() { return {read, write, erase, report}; }
};
Backend* Backend::active = nullptr;

ble_store_value record(uint8_t peer, uint8_t generation) {
    ble_store_value value{};
    value.sec.peer_addr.val[0] = peer;
    value.sec.ltk_present = 1;
    value.sec.irk_present = 1;
    value.sec.csrk_present = 1;
    value.sec.key_size = 16;
    value.sec.sc = 1;
    std::fill_n(value.sec.ltk, 16, generation);
    std::fill_n(value.sec.irk, 16, generation + 1);
    std::fill_n(value.sec.csrk, 16, generation + 2);
    return value;
}
void check_generation(const Backend& backend, int object, uint8_t peer, uint8_t generation) {
    ble_addr_t address{};
    address.val[0] = peer;
    const int slot = Backend::index(backend.ram[object], address);
    CHECK(slot >= 0);
    CHECK(backend.ram[object][slot].sec.ltk[0] == generation);
}

void updated_bonds_survive_reboot() {
    for (const int object : {BLE_STORE_OBJ_TYPE_OUR_SEC, BLE_STORE_OBJ_TYPE_PEER_SEC}) {
        Backend backend;
        auto adapter = backend.adapter();
        backend.seed(object, record(1, 10));
        backend.seed(object, record(2, 20));
        auto replacement = record(1, 30);
        CHECK(adapter.write(object, &replacement) == 0);
        CHECK(backend.writes == 1 && backend.deletes == 1);
        CHECK(backend.reports.size() == 1 && backend.reports[0].update);
        backend.reboot();
        check_generation(backend, object, 1, 30);
        check_generation(backend, object, 2, 20);
    }
}
void baseline_reproduces_sdk_gap() {
    Backend backend;
    backend.seed(BLE_STORE_OBJ_TYPE_OUR_SEC, record(1, 10));
    auto replacement = record(1, 30);
    CHECK(Backend::write(BLE_STORE_OBJ_TYPE_OUR_SEC, &replacement) == 0);
    backend.reboot();
    check_generation(backend, BLE_STORE_OBJ_TYPE_OUR_SEC, 1, 10);
}
void unchanged_records_avoid_flash_wear() {
    Backend backend;
    auto adapter = backend.adapter();
    auto initial = record(1, 10);
    initial.sec.irk_present = 0;
    initial.sec.csrk_present = 0;
    backend.seed(BLE_STORE_OBJ_TYPE_OUR_SEC, initial);
    auto same = initial;
    same.sec.bond_count = 500;
    same.sec.irk[0] ^= 0xff;
    same.sec.csrk[0] ^= 0xff;
    same.sec.sign_counter = 200;
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_OUR_SEC, &same) == 0);
    CHECK(backend.writes == 0 && backend.deletes == 0);
}
void security_metadata_is_persisted() {
    for (int variant = 0; variant < 9; ++variant) {
        Backend backend;
        auto adapter = backend.adapter();
        auto next = record(1, 10);
        backend.seed(BLE_STORE_OBJ_TYPE_PEER_SEC, next);
        switch (variant) {
        case 0: next.sec.key_size = 15; break;
        case 1: next.sec.ediv = 12; break;
        case 2: next.sec.rand_num = 43; break;
        case 3: next.sec.sc = 0; break;
        case 4: next.sec.authenticated = 1; break;
        case 5: next.sec.irk[1] ^= 0xff; break;
        case 6: next.sec.csrk[1] ^= 0xff; break;
        case 7: next.sec.sign_counter = 9; break;
        case 8: next.sec.irk_present = 0; break;
        }
        CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &next) == 0);
        CHECK(backend.writes == 1 && backend.deletes == 1);
        backend.reboot();
        CHECK(backend.ram[BLE_STORE_OBJ_TYPE_PEER_SEC][0].sec.key_size == next.sec.key_size);
        CHECK(backend.ram[BLE_STORE_OBJ_TYPE_PEER_SEC][0].sec.sign_counter == next.sec.sign_counter);
    }
}
void failed_updates_restore_and_latch() {
    for (bool deleting : {false, true}) {
        for (bool after_ram : {false, true}) {
            Backend backend;
            auto adapter = backend.adapter();
            backend.seed(BLE_STORE_OBJ_TYPE_OUR_SEC, record(1, 10));
            backend.seed(BLE_STORE_OBJ_TYPE_OUR_SEC, record(2, 20));
            auto replacement = record(1, 30);
            auto& faults = deleting ? backend.delete_faults : backend.write_faults;
            faults.push_back({1, BLE_HS_ESTORE_FAIL, after_ram});
            CHECK(adapter.write(BLE_STORE_OBJ_TYPE_OUR_SEC, &replacement) == BLE_HS_ESTORE_FAIL);
            CHECK(adapter.error() == BLE_HS_ESTORE_FAIL);
            CHECK(backend.reports.back().rollback == 0);
            const int operations = backend.writes + backend.deletes;
            CHECK(adapter.write(BLE_STORE_OBJ_TYPE_OUR_SEC, &replacement) == BLE_HS_ESTORE_FAIL);
            CHECK(backend.writes + backend.deletes == operations);
            backend.reboot();
            check_generation(backend, BLE_STORE_OBJ_TYPE_OUR_SEC, 1, 10);
            check_generation(backend, BLE_STORE_OBJ_TYPE_OUR_SEC, 2, 20);
        }
    }
}
void failed_new_bonds_are_latched() {
    for (bool after_ram : {false, true}) {
        Backend backend;
        auto adapter = backend.adapter();
        backend.write_faults = {{1, BLE_HS_ESTORE_FAIL, after_ram}};
        auto value = record(1, 10);
        CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value) == BLE_HS_ESTORE_FAIL);
        CHECK(adapter.error() == BLE_HS_ESTORE_FAIL);
        CHECK(!backend.reports.back().update);
        CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value) == BLE_HS_ESTORE_FAIL);
        CHECK(backend.writes == 1);
        backend.reboot();
        CHECK(backend.ram[BLE_STORE_OBJ_TYPE_PEER_SEC].empty());
    }
}
void rollback_failure_is_not_hidden() {
    Backend backend;
    auto adapter = backend.adapter();
    backend.seed(BLE_STORE_OBJ_TYPE_PEER_SEC, record(1, 10));
    backend.write_faults = {{1, BLE_HS_ESTORE_FAIL, true}, {2, BLE_HS_ESTORE_CAP, false}};
    auto replacement = record(1, 30);
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &replacement) == BLE_HS_ESTORE_FAIL);
    CHECK(adapter.error() == BLE_HS_ESTORE_FAIL);
    CHECK(backend.reports.back().rollback == BLE_HS_ESTORE_CAP);
}
void new_records_read_errors_and_passthrough() {
    Backend backend;
    auto adapter = backend.adapter();
    auto value = record(1, 10);
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_OUR_SEC, &value) == 0);
    CHECK(backend.writes == 1 && backend.deletes == 0);
    backend.reboot();
    check_generation(backend, BLE_STORE_OBJ_TYPE_OUR_SEC, 1, 10);
    backend.read_error = BLE_HS_ESTORE_FAIL;
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value) == BLE_HS_ESTORE_FAIL);
    CHECK(backend.deletes == 0);
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_CCCD, &value) == 0);
    CHECK(backend.writes == 2);
    adapter.reset();
    backend.read_error = 0;
    CHECK(adapter.write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value) == 0);
    CHECK(adapter.error() == 0);
}
} // namespace

int main() {
    baseline_reproduces_sdk_gap();
    updated_bonds_survive_reboot();
    unchanged_records_avoid_flash_wear();
    security_metadata_is_persisted();
    failed_updates_restore_and_latch();
    failed_new_bonds_are_latched();
    rollback_failure_is_not_hidden();
    new_records_read_errors_and_passthrough();
    std::cout << "BLE bond storage persistence and failure tests passed\n";
}
