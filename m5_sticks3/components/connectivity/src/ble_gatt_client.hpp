#pragma once
#include "ble_gateway.hpp"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"

namespace connectivity::gateway {
// Only reserve/query methods cross tasks. The NimBLE host owns driver calls
// and connection handles; the object outlives callbacks until host stop joins.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    int32_t reserve(const Request& request, uint32_t ticket);
    void reject(uint32_t ticket);
    Status status();
    ResultPage result(uint32_t ticket, size_t index, size_t offset);
    EventPage event(uint32_t after, uint32_t sequence, size_t offset);
    void connected(uint16_t connection);
    void disconnected();
    void execute(const Request& request, uint32_t ticket);
    bool expired(int64_t now);
    void notification(uint16_t connection, uint16_t handle, bool indication, os_mbuf* value);
    void security_complete(uint16_t connection, int status);
private:
    class Guard {
    public:
        explicit Guard(portMUX_TYPE& lock) : lock_(lock) { portENTER_CRITICAL(&lock_); }
        ~Guard() { portEXIT_CRITICAL(&lock_); }
    private:
        portMUX_TYPE& lock_;
    };
    // An immutable ticket rejects late callbacks after connection handle reuse.
    static Client* callback(uint16_t connection, void* argument);
    static int service_cb(uint16_t, const ble_gatt_error*, const ble_gatt_svc*, void*);
    static int characteristic_cb(uint16_t, const ble_gatt_error*, const ble_gatt_chr*, void*);
    static int descriptor_cb(uint16_t, const ble_gatt_error*, uint16_t, const ble_gatt_dsc*, void*);
    static int attribute_cb(uint16_t, const ble_gatt_error*, ble_gatt_attr*, void*);
    static int mtu_cb(uint16_t, const ble_gatt_error*, uint16_t, void*);
    bool discovery_status(int status);
    void finish(int status);
    void row(const Row& row);
    static Client* instance_;
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Store store_{};
    uint16_t connection_ = BLE_HS_CONN_HANDLE_NONE;
    uint32_t ticket_ = 0;
    Operation operation_ = Operation::services;
};
} // namespace connectivity::gateway
