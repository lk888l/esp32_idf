#include "sdkconfig.h"
#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "ble_gatt_client.hpp"
#include <algorithm>
#include "esp_timer.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

namespace connectivity::gateway {
Client* Client::instance_ = nullptr;
Client::Client() = default;
Client::~Client() { if (instance_ == this) instance_ = nullptr; }
int32_t Client::reserve(const Request& request, uint32_t ticket)
{ Guard guard(lock_); return store_.reserve(request, ticket); }
void Client::reject(uint32_t ticket)
{ Guard guard(lock_); store_.finish(ticket, transport_busy); }
Status Client::status() { Guard guard(lock_); return store_.status(); }
ResultPage Client::result(uint32_t ticket, size_t index, size_t offset)
{ Guard guard(lock_); return store_.result(ticket, index, offset); }
EventPage Client::event(uint32_t after, uint32_t sequence, size_t offset)
{ Guard guard(lock_); return store_.event(after, sequence, offset); }
void Client::connected(uint16_t connection)
{
    // Only the successfully reserved NimBLE host can deliver this callback.
    // Constructing a second inactive transport must not steal callback routing.
    instance_ = this;
    connection_ = connection; ticket_ = 0;
    Guard guard(lock_); store_.connect();
}
void Client::disconnected()
{
    connection_ = BLE_HS_CONN_HANDLE_NONE; ticket_ = 0;
    Guard guard(lock_); store_.disconnect();
}
bool Client::expired(int64_t now)
{
    Guard guard(lock_);
    if (!store_.expire(now)) return false;
    ticket_ = 0; connection_ = BLE_HS_CONN_HANDLE_NONE;
    return true;
}
void Client::finish(int status)
{ Guard guard(lock_); store_.finish(ticket_, status); }
void Client::row(const Row& row)
{ Guard guard(lock_); store_.add_row(ticket_, row); }
bool Client::discovery_status(int status)
{
    if (!status) return true;
    finish(status == BLE_HS_EDONE ? 0 : status); return false;
}
Client* Client::callback(uint16_t connection, void* argument)
{
    auto* self = instance_;
    const auto ticket = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument));
    if (!self || self->connection_ != connection || self->ticket_ != ticket) return nullptr;
    Guard guard(self->lock_);
    return self->store_.active(ticket) ? self : nullptr;
}
void Client::execute(const Request& request, uint32_t ticket)
{
    { Guard guard(lock_); if (!store_.start(ticket, esp_timer_get_time())) return; }
    ticket_ = ticket; operation_ = request.operation;
    void* argument = reinterpret_cast<void*>(static_cast<uintptr_t>(ticket));
    int status = 0;
    switch (request.operation) {
    case Operation::services:
        status = ble_gattc_disc_all_svcs(connection_, service_cb, argument); break;
    case Operation::characteristics:
        status = ble_gattc_disc_all_chrs(connection_, request.handle, request.end, characteristic_cb, argument); break;
    case Operation::descriptors:
        status = ble_gattc_disc_all_dscs(connection_, request.handle, request.end, descriptor_cb, argument); break;
    case Operation::read:
        status = ble_gattc_read_long(connection_, request.handle, 0, attribute_cb, argument); break;
    case Operation::write:
        if (request.length > ble_att_mtu(connection_) - 3) { finish(too_large); return; }
        status = ble_gattc_write_flat(connection_, request.handle, request.data, request.length, attribute_cb, argument); break;
    case Operation::subscribe: {
        const uint8_t value[] = {request.mode, 0};
        status = ble_gattc_write_flat(connection_, request.handle, value, sizeof(value), attribute_cb, argument); break;
    }
    case Operation::mtu:
        status = ble_gattc_exchange_mtu(connection_, mtu_cb, argument); break;
    case Operation::pair: {
        ble_gap_conn_desc description{};
        status = ble_gap_conn_find(connection_, &description);
        if (!status && description.sec_state.encrypted) { finish(0); return; }
        if (!status) status = ble_gap_security_initiate(connection_);
        break;
    }
    }
    if (status) finish(status);
}
int Client::service_cb(uint16_t connection, const ble_gatt_error* error, const ble_gatt_svc* service, void* argument)
{
    auto* self = callback(connection, argument);
    if (!self) return BLE_HS_EDONE;
    if (self->discovery_status(error->status) && service) {
        Row row{}; ble_uuid_to_str(&service->uuid.u, row.uuid);
        row.handle = service->start_handle; row.end = service->end_handle; self->row(row);
    }
    return 0;
}
int Client::characteristic_cb(uint16_t connection, const ble_gatt_error* error, const ble_gatt_chr* chr, void* argument)
{
    auto* self = callback(connection, argument);
    if (!self) return BLE_HS_EDONE;
    if (self->discovery_status(error->status) && chr) {
        Row row{}; ble_uuid_to_str(&chr->uuid.u, row.uuid);
        row.handle = chr->def_handle; row.value_handle = chr->val_handle; row.properties = chr->properties; self->row(row);
    }
    return 0;
}
int Client::descriptor_cb(uint16_t connection, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void* argument)
{
    auto* self = callback(connection, argument);
    if (!self) return BLE_HS_EDONE;
    if (self->discovery_status(error->status) && dsc) {
        Row row{}; ble_uuid_to_str(&dsc->uuid.u, row.uuid); row.handle = dsc->handle; self->row(row);
    }
    return 0;
}
int Client::attribute_cb(uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attr, void* argument)
{
    auto* self = callback(connection, argument);
    if (!self) return BLE_HS_EDONE;
    if (self->operation_ != Operation::read) { self->finish(error->status); return 0; }
    if (error->status) { self->finish(error->status == BLE_HS_EDONE ? 0 : error->status); return 0; }
    if (!attr || !attr->om) { self->finish(invalid_request); return BLE_HS_EDONE; }
    uint8_t data[kValueBytes]{};
    const size_t size = OS_MBUF_PKTLEN(attr->om);
    if (size > sizeof(data)) { self->finish(too_large); return BLE_HS_EDONE; }
    if (os_mbuf_copydata(attr->om, 0, size, data)) { self->finish(invalid_request); return BLE_HS_EDONE; }
    Guard guard(self->lock_);
    return self->store_.append(self->ticket_, attr->offset, data, size) ? 0 : BLE_HS_EDONE;
}
int Client::mtu_cb(uint16_t connection, const ble_gatt_error* error, uint16_t mtu, void* argument)
{
    if (auto* self = callback(connection, argument)) {
        if (!error->status) {
            const uint8_t value[] = {static_cast<uint8_t>(mtu), static_cast<uint8_t>(mtu >> 8)};
            Guard guard(self->lock_); self->store_.append(self->ticket_, 0, value, sizeof(value));
        }
        self->finish(error->status);
    }
    return 0;
}
void Client::notification(uint16_t connection, uint16_t handle, bool indication, os_mbuf* value)
{
    if (connection != connection_ || !value) return;
    uint8_t data[kValueBytes]{};
    const size_t length = OS_MBUF_PKTLEN(value);
    if (os_mbuf_copydata(value, 0, std::min(length, sizeof(data)), data)) return;
    Guard guard(lock_); store_.notify(handle, indication, data, length);
}
void Client::security_complete(uint16_t connection, int status)
{
    if (connection == connection_ && operation_ == Operation::pair) finish(status);
}
} // namespace connectivity::gateway
#endif
