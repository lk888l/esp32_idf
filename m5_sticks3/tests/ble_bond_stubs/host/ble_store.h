#pragma once
#include <cstdint>
#include <cstring>

#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_CCCD 3

struct ble_addr_t { uint8_t type; uint8_t val[6]; };
inline int ble_addr_cmp(const ble_addr_t* a, const ble_addr_t* b) {
    return a->type != b->type ? 1 : std::memcmp(a->val, b->val, sizeof(a->val));
}
struct ble_store_key_sec { ble_addr_t peer_addr; uint8_t idx; };
struct ble_store_value_sec {
    ble_addr_t peer_addr;
    uint16_t bond_count;
    uint8_t key_size;
    uint16_t ediv;
    uint64_t rand_num;
    uint8_t ltk[16];
    uint8_t ltk_present : 1;
    uint8_t irk[16];
    uint8_t irk_present : 1;
    uint8_t csrk[16];
    uint8_t csrk_present : 1;
    uint32_t sign_counter;
    unsigned authenticated : 1;
    uint8_t sc : 1;
};
union ble_store_key { ble_store_key_sec sec; };
union ble_store_value { ble_store_value_sec sec; };
typedef int ble_store_read_fn(int, const ble_store_key*, ble_store_value*);
typedef int ble_store_write_fn(int, const ble_store_value*);
typedef int ble_store_delete_fn(int, const ble_store_key*);
inline void ble_store_key_from_value_sec(ble_store_key_sec* key,
                                         const ble_store_value_sec* value) {
    *key = {};
    key->peer_addr = value->peer_addr;
}
