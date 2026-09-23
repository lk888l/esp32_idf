#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;
constexpr int BLE_HS_EDONE = 14;
struct ble_uuid_t { uint16_t value = 0; };
struct ble_uuid_any_t { ble_uuid_t u{}; };
struct os_mbuf { std::vector<uint8_t> data; };
#define OS_MBUF_PKTLEN(value) ((value)->data.size())
inline int os_mbuf_copydata(os_mbuf* value, size_t offset, size_t size, void* output)
{
    if (offset > value->data.size() || size > value->data.size() - offset) return 1;
    std::memcpy(output, value->data.data() + offset, size); return 0;
}
struct ble_gatt_error { uint16_t status = 0, att_handle = 0; };
struct ble_gatt_attr { uint16_t handle = 0, offset = 0; os_mbuf* om = nullptr; };
struct ble_gatt_svc { uint16_t start_handle = 0, end_handle = 0; ble_uuid_any_t uuid{}; };
struct ble_gatt_chr { uint16_t def_handle = 0, val_handle = 0; uint8_t properties = 0; ble_uuid_any_t uuid{}; };
struct ble_gatt_dsc { uint16_t handle = 0; ble_uuid_any_t uuid{}; };
using ble_gatt_attr_fn = int(uint16_t, const ble_gatt_error*, ble_gatt_attr*, void*);
using ble_gatt_disc_svc_fn = int(uint16_t, const ble_gatt_error*, const ble_gatt_svc*, void*);
using ble_gatt_chr_fn = int(uint16_t, const ble_gatt_error*, const ble_gatt_chr*, void*);
using ble_gatt_dsc_fn = int(uint16_t, const ble_gatt_error*, uint16_t, const ble_gatt_dsc*, void*);
using ble_gatt_mtu_fn = int(uint16_t, const ble_gatt_error*, uint16_t, void*);
namespace gatt_fake {
inline void* argument = nullptr;
inline ble_gatt_attr_fn* attribute = nullptr;
inline ble_gatt_disc_svc_fn* service = nullptr;
inline ble_gatt_chr_fn* characteristic = nullptr;
inline ble_gatt_dsc_fn* descriptor = nullptr;
inline ble_gatt_mtu_fn* mtu_callback = nullptr;
inline int result = 0, writes = 0;
inline uint16_t mtu = 23, last_handle = 0;
inline bool encrypted = false;
inline std::vector<uint8_t> written;
}
inline int ble_gattc_disc_all_svcs(uint16_t, ble_gatt_disc_svc_fn* cb, void* arg)
{ gatt_fake::service = cb; gatt_fake::argument = arg; return gatt_fake::result; }
inline int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn* cb, void* arg)
{ gatt_fake::characteristic = cb; gatt_fake::argument = arg; return gatt_fake::result; }
inline int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn* cb, void* arg)
{ gatt_fake::descriptor = cb; gatt_fake::argument = arg; return gatt_fake::result; }
inline int ble_gattc_read_long(uint16_t, uint16_t, uint16_t, ble_gatt_attr_fn* cb, void* arg)
{ gatt_fake::attribute = cb; gatt_fake::argument = arg; return gatt_fake::result; }
inline int ble_gattc_write_flat(uint16_t, uint16_t handle, const void* data, uint16_t size, ble_gatt_attr_fn* cb, void* arg)
{
    gatt_fake::last_handle = handle; ++gatt_fake::writes;
    const auto* bytes = static_cast<const uint8_t*>(data);
    gatt_fake::written.assign(bytes, bytes + size);
    gatt_fake::attribute = cb; gatt_fake::argument = arg; return gatt_fake::result;
}
inline int ble_gattc_exchange_mtu(uint16_t, ble_gatt_mtu_fn* cb, void* arg)
{ gatt_fake::mtu_callback = cb; gatt_fake::argument = arg; return gatt_fake::result; }
inline uint16_t ble_att_mtu(uint16_t) { return gatt_fake::mtu; }
struct ble_gap_conn_desc { struct { bool encrypted; } sec_state; };
inline int ble_gap_conn_find(uint16_t, ble_gap_conn_desc* description)
{ description->sec_state.encrypted = gatt_fake::encrypted; return gatt_fake::result; }
inline int ble_gap_security_initiate(uint16_t) { return gatt_fake::result; }
inline void ble_uuid_to_str(const ble_uuid_t*, char* output) { std::strcpy(output, "0x2902"); }
