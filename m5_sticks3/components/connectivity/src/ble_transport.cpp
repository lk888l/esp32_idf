#include "ble_transport.hpp"

#include "sdkconfig.h"

#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "app_task.hpp"
#include "ble_bond_store.hpp"
#include "connectivity_policy.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

// IDF's store/config header omits this initialization entry point.
extern "C" void ble_store_config_init(void);

namespace connectivity {
namespace {

constexpr char kTag[] = "ble_transport";
// ATT limits an attribute value to 512 octets, independent of transport policy.
constexpr size_t kAttributeBytes = 512;
constexpr size_t kResponseBytes = kMaxResponseBytes < kAttributeBytes
    ? kMaxResponseBytes : kAttributeBytes;
constexpr size_t kMaxNameBytes = 29; // Complete name in a legacy scan response.
constexpr ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
constexpr ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
constexpr ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

} // namespace

struct BleTransport::Impl final : AppTask {
    Impl()
        : AppTask("ble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE,
                  configMAX_PRIORITIES - 4, CONFIG_BT_NIMBLE_PINNED_TO_CORE)
    {
        characteristics[0].uuid = &kRxUuid.u;
        characteristics[0].access_cb = access;
        characteristics[0].arg = this;
        // Commands use Write Request, so malformed or oversized frames return
        // an ATT error to the client instead of silently losing a command.
        characteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
        characteristics[0].min_key_size = 16;
        characteristics[1].uuid = &kTxUuid.u;
        characteristics[1].access_cb = access;
        characteristics[1].arg = this;
        characteristics[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                   BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC;
        characteristics[1].min_key_size = 16;
        characteristics[1].val_handle = &tx_handle;
        services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
        services[0].uuid = &kServiceUuid.u;
        services[0].characteristics = characteristics;
        clear_response();
        commands = xQueueCreateStatic(kCommandCapacity, sizeof(Command),
                                      command_storage, &command_control);
    }

    void main() override { nimble_port_run(); }

    template <typename Update>
    void update_state(Update update)
    {
        portENTER_CRITICAL(&state_lock);
        update(state);
        portEXIT_CRITICAL(&state_lock);
    }

    BleSnapshot snapshot()
    {
        portENTER_CRITICAL(&state_lock);
        const BleSnapshot result = state;
        portEXIT_CRITICAL(&state_lock);
        return result;
    }


    enum class CommandKind : uint8_t { scan, connect, disconnect, enable };
    struct Command {
        CommandKind kind{};
        ble_addr_t address{};
        char peer_name[32]{};
        bool enabled = false;
    };
    static constexpr UBaseType_t kCommandCapacity = 4;
    // Bound address accounting independently from the strongest-16 result list.
    static constexpr size_t kTrackedAddresses = 128;

    template <typename Update>
    void update_diagnostics(Update update)
    {
        portENTER_CRITICAL(&state_lock);
        update(analyzer);
        portEXIT_CRITICAL(&state_lock);
    }

    BleDiagnostics diagnostics()
    {
        portENTER_CRITICAL(&state_lock);
        const BleDiagnostics result = analyzer;
        portEXIT_CRITICAL(&state_lock);
        return result;
    }


    bool is_scanning()
    {
        portENTER_CRITICAL(&state_lock);
        const bool result = analyzer.scanning;
        portEXIT_CRITICAL(&state_lock);
        return result;
    }

    bool is_connecting()
    {
        portENTER_CRITICAL(&state_lock);
        const bool result = analyzer.connecting;
        portEXIT_CRITICAL(&state_lock);
        return result;
    }

    void refresh_enabled()
    {
        bool active_radio = radio_enabled.load(std::memory_order_acquire) ||
                            ble_gap_adv_active() ||
                            connection != BLE_HS_CONN_HANDLE_NONE ||
                            peer_connection != BLE_HS_CONN_HANDLE_NONE;
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
        active_radio = active_radio || ble_gap_disc_active();
#endif
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
        active_radio = active_radio || ble_gap_conn_active();
#endif
        // Disabled becomes visible only after advertising, scan/initiation and
        // both links are actually stopped. Failures remain visible as enabled.
        update_state([active_radio](BleSnapshot& value) { value.enabled = active_radio; });
        update_diagnostics([active_radio](BleDiagnostics& value) { value.enabled = active_radio; });
    }

    static void format_address(const ble_addr_t& address, char (&text)[18])
    {
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                      address.val[5], address.val[4], address.val[3],
                      address.val[2], address.val[1], address.val[0]);
    }

    static bool parse_address(const char* text, uint8_t type, ble_addr_t& result)
    {
        if (text == nullptr || strnlen(text, 18) != 17 || type > BLE_ADDR_RANDOM_ID) {
            return false;
        }
        result.type = type;
        for (unsigned index = 0; index < BLE_DEV_ADDR_LEN; ++index) {
            const size_t offset = index * 3;
            const auto digit = [](char value) -> int {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'A' && value <= 'F') return value - 'A' + 10;
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                return -1;
            };
            const int high = digit(text[offset]);
            const int low = digit(text[offset + 1]);
            if (high < 0 || low < 0 || (index < 5 && text[offset + 2] != ':')) {
                return false;
            }
            result.val[5 - index] = static_cast<uint8_t>((high << 4) | low);
        }
        return true;
    }

    esp_err_t enqueue(const Command& command)
    {
        if (!initialized || stopping.load(std::memory_order_acquire) ||
            !host_synced.load(std::memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        return xQueueSend(commands, &command, 0) == pdTRUE
            ? ESP_OK : ESP_ERR_NO_MEM;
    }

    void record_advertisement(const ble_gap_disc_desc& report)
    {
        BleDevice device{};
        format_address(report.addr, device.address);
        device.address_type = report.addr.type;
        device.rssi = report.rssi == 127 ? -127 : report.rssi;
        device.connectable = report.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                             report.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        ble_hs_adv_fields fields{};
        if (ble_hs_adv_parse_fields(&fields, report.data, report.length_data) == 0 &&
            fields.name != nullptr) {
            const size_t length = std::min<size_t>(fields.name_len, sizeof(device.name) - 1);
            for (size_t index = 0; index < length; ++index) {
                const uint8_t byte = fields.name[index];
                // Advertisements are untrusted binary data; the small built-in
                // display font is ASCII. Never forward control bytes to it.
                device.name[index] = byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '?';
            }
        }
        bool known = false;
        for (size_t index = 0; index < seen_count; ++index) {
            if (ble_addr_cmp(&seen_addresses[index], &report.addr) == 0) {
                known = true;
                break;
            }
        }
        if (!known && seen_count < kTrackedAddresses) {
            seen_addresses[seen_count++] = report.addr;
        }
        update_diagnostics([&](BleDiagnostics& value) {
            // Saturates at the fixed address-accounting capacity. The list
            // still retains the strongest devices if more addresses appear.
            value.total_found = static_cast<uint16_t>(seen_count);
            size_t slot = value.count;
            for (size_t index = 0; index < value.count; ++index) {
                const auto& existing = value.devices[index];
                if (existing.address_type == device.address_type &&
                    std::strcmp(existing.address, device.address) == 0) {
                    slot = index;
                    break;
                }
            }
            if (slot < value.count) {
                const auto& previous = value.devices[slot];
                if (device.name[0] == '\0') {
                    std::memcpy(device.name, previous.name, sizeof(device.name));
                }
                // A scan response does not carry the original connectable bit.
                if (report.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
                    device.connectable = previous.connectable;
                }
            } else if (value.count < kMaxBleDevices) {
                ++value.count;
            } else {
                if (device.rssi <= value.devices[value.count - 1].rssi) {
                    return;
                }
                slot = value.count - 1;
            }
            value.devices[slot] = device;
            while (slot > 0 && value.devices[slot].rssi > value.devices[slot - 1].rssi) {
                std::swap(value.devices[slot], value.devices[slot - 1]);
                --slot;
            }
            while (slot + 1 < value.count &&
                   value.devices[slot].rssi < value.devices[slot + 1].rssi) {
                std::swap(value.devices[slot], value.devices[slot + 1]);
                ++slot;
            }
        });
    }

    void begin_scan()
    {
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
        if (!radio_enabled.load(std::memory_order_acquire) ||
            is_connecting() || ble_gap_disc_active()) {
            update_diagnostics([](BleDiagnostics& value) { value.scan_error = BLE_HS_EBUSY; });
            return;
        }
        seen_count = 0;
        update_diagnostics([](BleDiagnostics& value) {
            value.scanning = true;
            value.count = 0;
            value.total_found = 0;
            value.scan_error = 0;
            ++value.generation;
            for (auto& device : value.devices) device = BleDevice{};
        });
        ble_gap_disc_params parameters{};
        parameters.itvl = 160;   // 100 ms interval; coexist with Wi-Fi and UI.
        parameters.window = 80;  // 50 ms active window.
        parameters.passive = 0;  // Collect names advertised in scan responses.
        parameters.filter_duplicates = 1;
        const int result = ble_gap_disc(address_type, 8000, &parameters,
                                         analyzer_gap_event, this);
        if (result != 0) {
            update_diagnostics([result](BleDiagnostics& value) {
                value.scanning = false;
                value.scan_error = result;
            });
        }
#else
        update_diagnostics([](BleDiagnostics& value) { value.scan_error = BLE_HS_ENOTSUP; });
#endif
    }

    void begin_connect(const Command& command)
    {
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
        if (!radio_enabled.load(std::memory_order_acquire) ||
            peer_connection != BLE_HS_CONN_HANDLE_NONE || is_connecting()) {
            update_diagnostics([](BleDiagnostics& value) { value.peer_error = BLE_HS_EBUSY; });
            return;
        }
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
        if (ble_gap_disc_active()) {
            const int result = ble_gap_disc_cancel();
            if (result != 0 && result != BLE_HS_EALREADY) {
                update_diagnostics([result](BleDiagnostics& value) { value.peer_error = result; });
                return;
            }
            update_diagnostics([](BleDiagnostics& value) { value.scanning = false; });
        }
#endif
        char address[18]{};
        format_address(command.address, address);
        update_diagnostics([&](BleDiagnostics& value) {
            value.connecting = true;
            value.peer_connected = false;
            value.peer_error = 0;
            value.peer_rssi = 0;
            value.peer_mtu = 23;
            value.service_count = 0;
            std::memcpy(value.peer_address, address, sizeof(address));
            std::memcpy(value.peer_name, command.peer_name, sizeof(value.peer_name));
            std::memset(value.services, 0, sizeof(value.services));
        });
        // Only establish the user-selected link and inspect primary services;
        // never write arbitrary remote characteristics or subscribe to them.
        peer_connect_allowed = true;
        peer_disconnect_requested = false;
        const int result = ble_gap_connect(address_type, &command.address, 10000,
                                            nullptr, analyzer_gap_event, this);
        if (result != 0) {
            peer_connect_allowed = false;
            update_diagnostics([result](BleDiagnostics& value) {
                value.connecting = false;
                value.peer_error = result;
            });
        }
#else
        update_diagnostics([](BleDiagnostics& value) { value.peer_error = BLE_HS_ENOTSUP; });
#endif
    }

    void end_peer_connection()
    {
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
        peer_connect_allowed = false;
        int result = 0;
        if (is_connecting()) {
            result = ble_gap_conn_cancel();
            if (result == 0) peer_disconnect_requested = true;
            if (result == BLE_HS_EALREADY) result = 0;
        } else if (peer_connection != BLE_HS_CONN_HANDLE_NONE) {
            result = ble_gap_terminate(peer_connection, BLE_ERR_REM_USER_CONN_TERM);
            if (result == 0) peer_disconnect_requested = true;
            if (result == BLE_HS_ENOTCONN) result = 0;
        }
        if (result != 0) {
            update_diagnostics([result](BleDiagnostics& value) { value.peer_error = result; });
        }
#endif
    }

    void change_enabled(bool enabled)
    {
        if (bond_store.error() != 0) enabled = false;
        radio_enabled.store(enabled, std::memory_order_release);
        if (enabled) {
            if (connection == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
                advertise();
            }
            refresh_enabled();
            return;
        }
        if (ble_gap_adv_active()) {
            const int result = ble_gap_adv_stop();
            if (result == 0 || result == BLE_HS_EALREADY) {
                update_state([](BleSnapshot& value) { value.advertising = false; });
            } else {
                update_diagnostics([result](BleDiagnostics& value) { value.peer_error = result; });
            }
        }
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
        if (ble_gap_disc_active()) {
            const int result = ble_gap_disc_cancel();
            update_diagnostics([result](BleDiagnostics& value) {
                value.scanning = result != 0 && result != BLE_HS_EALREADY;
                if (value.scanning) value.scan_error = result;
            });
        }
#endif
        end_peer_connection();
        if (connection != BLE_HS_CONN_HANDLE_NONE) {
            const int result = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
            if (result != 0 && result != BLE_HS_ENOTCONN) {
                update_diagnostics([result](BleDiagnostics& value) { value.peer_error = result; });
            }
        }
        refresh_enabled();
    }

    static int service_discovered(uint16_t handle, const ble_gatt_error* error,
                                  const ble_gatt_svc* service, void* argument)
    {
        auto* self = static_cast<Impl*>(argument);
        if (handle != self->peer_connection || self->stopping.load(std::memory_order_acquire)) {
            return BLE_HS_EDONE;
        }
        if (error->status == BLE_HS_EDONE) {
            ESP_LOGI(kTag, "analyzer service discovery complete; retained=%u",
                     static_cast<unsigned>(self->diagnostics().service_count));
            return 0;
        }
        if (error->status != 0) {
            ESP_LOGW(kTag, "analyzer service discovery failed: %d", error->status);
            self->update_diagnostics([error](BleDiagnostics& value) {
                value.peer_error = error->status;
            });
            return 0;
        }
        if (service != nullptr) {
            char uuid[BLE_UUID_STR_LEN]{};
            ble_uuid_to_str(&service->uuid.u, uuid);
            ESP_LOGI(kTag, "analyzer service %s handles=%u-%u", uuid,
                     service->start_handle, service->end_handle);
            self->update_diagnostics([&](BleDiagnostics& value) {
                if (value.service_count < kMaxBleServices) {
                    std::snprintf(value.services[value.service_count++],
                                  sizeof(value.services[0]), "%s", uuid);
                }
            });
        }
        return 0;
    }

    void discover_services(uint16_t handle)
    {
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
        const int result = ble_gattc_disc_all_svcs(handle, service_discovered, this);
        ESP_LOGI(kTag, "analyzer service discovery started; handle=%u rc=%d", handle, result);
        if (result != 0) {
            update_diagnostics([result](BleDiagnostics& value) { value.peer_error = result; });
        }
#endif
    }

    static int analyzer_gap_event(ble_gap_event* event, void* argument)
    {
        auto* self = static_cast<Impl*>(argument);
        switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (self->radio_enabled.load(std::memory_order_acquire) &&
                self->is_scanning()) {
                self->record_advertisement(event->disc);
            }
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            self->update_diagnostics([event](BleDiagnostics& value) {
                value.scanning = false;
                value.scan_error = event->disc_complete.reason;
            });
            break;
        case BLE_GAP_EVENT_CONNECT:
            self->update_diagnostics([event, self](BleDiagnostics& value) {
                value.connecting = false;
                // The stack reports an accepted application cancellation as
                // EAPP. Preserve every unrelated connect failure verbatim.
                value.peer_error = self->peer_disconnect_requested &&
                                   event->connect.status == BLE_HS_EAPP
                    ? 0 : event->connect.status;
            });
            self->peer_disconnect_requested = false;
            if (event->connect.status == 0) {
                if (self->stopping.load(std::memory_order_acquire) ||
                    !self->radio_enabled.load(std::memory_order_acquire) ||
                    !self->peer_connect_allowed ||
                    self->peer_connection != BLE_HS_CONN_HANDLE_NONE) {
                    ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                self->peer_connect_allowed = false;
                self->peer_connection = event->connect.conn_handle;
                const uint16_t mtu = ble_att_mtu(self->peer_connection);
                self->update_diagnostics([mtu](BleDiagnostics& value) {
                    value.peer_connected = true;
                    value.peer_mtu = mtu;
                });
                // Primary service discovery works at the default MTU of 23.
                // Do not gate read-only diagnostics on an optional exchange:
                // an unresponsive MTU request can stall ATT for 30 seconds.
                // A peer-initiated MTU exchange is reflected by GAP_MTU below.
                self->discover_services(self->peer_connection);
                ESP_LOGI(kTag, "analyzer peer connected");
            } else {
                self->peer_connect_allowed = false;
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle == self->peer_connection) {
                const bool expected = self->peer_disconnect_requested &&
                    event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL);
                self->peer_disconnect_requested = false;
                self->peer_connection = BLE_HS_CONN_HANDLE_NONE;
                self->update_diagnostics([event, expected](BleDiagnostics& value) {
                    value.peer_connected = false;
                    value.connecting = false;
                    value.peer_rssi = 0;
                    value.peer_mtu = 23;
                    // Timeout, MIC failure and remote termination remain
                    // errors even if a disconnect was requested concurrently.
                    value.peer_error = expected ? 0 : event->disconnect.reason;
                });
                ESP_LOGI(kTag, "analyzer peer disconnected: %d", event->disconnect.reason);
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE: {
            if (self->enforce_store_health(event->enc_change.conn_handle)) break;
            ble_gap_conn_desc description{};
            const int result = ble_gap_conn_find(event->enc_change.conn_handle, &description);
            if (result == 0 && description.role == BLE_GAP_ROLE_MASTER) {
                ESP_LOGI(kTag, "analyzer security status=%d encrypted=%u bonded=%u key_bytes=%u",
                         event->enc_change.status,
                         static_cast<unsigned>(description.sec_state.encrypted),
                         static_cast<unsigned>(description.sec_state.bonded),
                         static_cast<unsigned>(description.sec_state.key_size));
            }
            break;
        }
        case BLE_GAP_EVENT_MTU:
            if (event->mtu.conn_handle == self->peer_connection) {
                self->update_diagnostics([event](BleDiagnostics& value) {
                    value.peer_mtu = event->mtu.value;
                });
            }
            break;
        default:
            break;
        }
        return 0;
    }

    static void process_host_event(ble_npl_event* event)
    {
        auto* self = static_cast<Impl*>(ble_npl_event_get_arg(event));
        if (self->stopping.load(std::memory_order_acquire) ||
            !self->host_synced.load(std::memory_order_acquire)) {
            self->host_event_pending.store(false, std::memory_order_release);
            return;
        }
        if (self->enforce_store_health()) {
            xQueueReset(self->commands);
            self->host_event_pending.store(false, std::memory_order_release);
            return;
        }
        Command command{};
        for (UBaseType_t index = 0; index < kCommandCapacity &&
             xQueueReceive(self->commands, &command, 0) == pdTRUE; ++index) {
            switch (command.kind) {
            case CommandKind::scan: self->begin_scan(); break;
            case CommandKind::connect: self->begin_connect(command); break;
            case CommandKind::disconnect: self->end_peer_connection(); break;
            case CommandKind::enable: self->change_enabled(command.enabled); break;
            }
        }
        self->refresh_enabled();
        if (self->peer_connection != BLE_HS_CONN_HANDLE_NONE) {
            int8_t rssi = 0;
            const int result = ble_gap_conn_rssi(self->peer_connection, &rssi);
            if (result == 0) {
                self->update_diagnostics([rssi](BleDiagnostics& value) {
                    value.peer_rssi = rssi == 127 ? 0 : rssi;
                });
            }
        }
        self->host_event_pending.store(false, std::memory_order_release);
    }

    void clear_response()
    {
        // Never expose the previous peer's command response to a new peer.
        std::memset(response, 0, sizeof(response));
        std::strcpy(response, "{\"error\":\"no_response\"}");
        response_length = std::strlen(response);
    }

    void advertise()
    {
        if (stopping.load(std::memory_order_acquire) ||
            !radio_enabled.load(std::memory_order_acquire) ||
            connection != BLE_HS_CONN_HANDLE_NONE) {
            return;
        }
        // Privacy preemption recovery and DISCONNECT may both request a
        // restart. An existing advertisement already satisfies that request.
        if (ble_gap_adv_active()) {
            update_state([](BleSnapshot& value) { value.advertising = true; });
            return;
        }
        ble_hs_adv_fields fields{};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.uuids128 = &kServiceUuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
        int result = ble_gap_adv_set_fields(&fields);
        if (result == 0) {
            ble_hs_adv_fields scan_response{};
            scan_response.name = reinterpret_cast<uint8_t*>(name);
            scan_response.name_len = std::strlen(name);
            scan_response.name_is_complete = 1;
            result = ble_gap_adv_rsp_set_fields(&scan_response);
        }
        if (result == 0) {
            ble_gap_adv_params parameters{};
            parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
            parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
            result = ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER,
                                       &parameters, gap_event, this);
        }
        const bool advertising = ble_gap_adv_active() != 0;
        update_state([advertising](BleSnapshot& value) { value.advertising = advertising; });
        if (result != 0 && !(result == BLE_HS_EALREADY && advertising)) {
            ESP_LOGE(kTag, "advertising failed: %d", result);
        }
    }

    static void on_sync()
    {
        Impl* self = active.load(std::memory_order_acquire);
        if (self == nullptr || self->stopping.load(std::memory_order_acquire)) {
            return;
        }
        // Dynamic privacy startup initializes the default store before sync.
        // Install here so that initialization cannot overwrite the decorator.
        ble_hs_cfg.store_write_cb = Impl::write_bond_store;
        int result = ble_hs_util_ensure_addr(0);
        if (result == 0) {
            result = ble_hs_id_infer_auto(0, &self->address_type);
        }
        if (result != 0) {
            ESP_LOGE(kTag, "address initialization failed: %d", result);
            return;
        }
        self->host_synced.store(true, std::memory_order_release);
        self->advertise();
    }

    static void on_reset(int reason)
    {
        Impl* self = active.load(std::memory_order_acquire);
        if (self == nullptr) {
            return;
        }
        self->host_synced.store(false, std::memory_order_release);
        self->connection = BLE_HS_CONN_HANDLE_NONE;
        self->peer_connection = BLE_HS_CONN_HANDLE_NONE;
        self->peer_connect_allowed = false;
        self->peer_disconnect_requested = false;
        self->update_diagnostics([reason](BleDiagnostics& value) {
            value.scanning = false;
            value.connecting = false;
            value.peer_connected = false;
            value.peer_mtu = 23;
            value.peer_rssi = 0;
            value.peer_error = reason;
        });
        self->clear_response();
        self->update_state([](BleSnapshot& value) {
            value.advertising = false;
            value.connected = false;
            value.subscribed = false;
            value.encrypted = false;
            value.bonded = false;
            value.mtu = 23;
        });
        // The host invokes on_sync again after recovering from a reset.
        ESP_LOGW(kTag, "host reset: %d", reason);
    }

    bool adopt_connection(uint16_t handle)
    {
        if (stopping.load(std::memory_order_acquire) ||
            !radio_enabled.load(std::memory_order_acquire) || bond_store.error() != 0) {
            return false;
        }
        if (connection == handle) {
            return true;
        }
        if (connection != BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        ble_gap_conn_desc description{};
        if (ble_gap_conn_find(handle, &description) != 0 ||
            description.role != BLE_GAP_ROLE_SLAVE) {
            // Analyzer central links must never become the NUS server client.
            return false;
        }
        // IDF can restore encryption and CCCDs before its delayed CONNECT
        // callback. Initialize this live connection exactly once, preserving
        // real subscription events when CONNECT subsequently arrives.
        connection = handle;
        clear_response();
        const uint16_t mtu = ble_att_mtu(handle);
        const bool encrypted = description.sec_state.encrypted != 0;
        const bool bonded = description.sec_state.bonded != 0;
        update_state([mtu, encrypted, bonded](BleSnapshot& value) {
            value.advertising = false;
            value.connected = true;
            value.subscribed = false;
            value.encrypted = encrypted;
            value.bonded = bonded;
            value.mtu = mtu;
        });
        return true;
    }

    static int gap_event(ble_gap_event* event, void* argument)
    {
        auto* self = static_cast<Impl*>(argument);
        switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            self->update_state([](BleSnapshot& value) { value.advertising = false; });
            if (event->connect.status == 0) {
                if (!self->adopt_connection(event->connect.conn_handle)) {
                    ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                ESP_LOGI(kTag, "client connected");
            } else {
                self->advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle != self->connection) {
                break;
            }
            self->connection = BLE_HS_CONN_HANDLE_NONE;
            self->clear_response();
            self->update_state([](BleSnapshot& value) {
                value.connected = false;
                value.subscribed = false;
                value.encrypted = false;
                value.bonded = false;
                value.mtu = 23;
            });
            ESP_LOGI(kTag, "client disconnected: %d", event->disconnect.reason);
            self->advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self->update_state([](BleSnapshot& value) { value.advertising = false; });
            if (self->connection == BLE_HS_CONN_HANDLE_NONE) {
                self->advertise();
            }
            break;
        case BLE_GAP_EVENT_SUBSCRIBE: {
            const auto& subscription = event->subscribe;
            if (self->enforce_store_health(subscription.conn_handle)) break;
            if (subscription.attr_handle != self->tx_handle) {
                break;
            }
            if (subscription.reason == BLE_GAP_SUBSCRIBE_REASON_TERM) {
                if (subscription.conn_handle != self->connection) {
                    break;
                }
            } else if (!self->adopt_connection(subscription.conn_handle)) {
                break;
            }
            const bool subscribed = subscription.cur_notify != 0;
            self->update_state([subscribed](BleSnapshot& value) {
                value.subscribed = subscribed;
            });
            ESP_LOGI(kTag, "notifications enabled=%u reason=%u",
                     static_cast<unsigned>(subscribed),
                     static_cast<unsigned>(subscription.reason));
            break;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            if (self->enforce_store_health(event->enc_change.conn_handle)) break;
            if (!self->adopt_connection(event->enc_change.conn_handle)) {
                break;
            }
            ble_gap_conn_desc description{};
            const int result = ble_gap_conn_find(self->connection, &description);
            const bool encrypted = result == 0 && description.sec_state.encrypted;
            const bool bonded = result == 0 && description.sec_state.bonded;
            self->update_state([encrypted, bonded](BleSnapshot& value) {
                value.encrypted = encrypted;
                value.bonded = bonded;
            });
            ESP_LOGI(kTag, "security status=%d encrypted=%u bonded=%u key_bytes=%u",
                     event->enc_change.status, static_cast<unsigned>(encrypted),
                     static_cast<unsigned>(bonded),
                     static_cast<unsigned>(description.sec_state.key_size));
            break;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // Windows may discard its bond when the user removes a device.
            // Replace only this peer's stale record, and never downgrade SC,
            // key length, or an existing authenticated bond.
            const auto& repeat = event->repeat_pairing;
            const bool stopping = self->stopping.load(std::memory_order_acquire);
            const bool was_tracked = repeat.conn_handle == self->connection;
            // SMP may request replacement before IDF delivers CONNECT. Apply
            // the same live peripheral-role and single-peer validation used
            // by early encryption/CCCD callbacks before checking bond policy.
            const bool matches = self->adopt_connection(repeat.conn_handle);
            const bool auth_downgrade = repeat.cur_authenticated && !repeat.new_authenticated;
            if (matches && !was_tracked) {
                ESP_LOGI(kTag, "adopted early pairing link; handle=%u",
                         static_cast<unsigned>(repeat.conn_handle));
            }
            const char* ignored = stopping ? "transport stopping"
                : !matches ? "handle is not the tracked peripheral link"
                : !repeat.new_sc ? "Secure Connections required"
                : repeat.new_key_size != 16 ? "128-bit key required"
                : !repeat.new_bonding ? "bonding required"
                : auth_downgrade ? "authenticated bond downgrade"
                : nullptr;
            if (ignored != nullptr) {
                ESP_LOGW(kTag, "repeat pairing ignored: %s", ignored);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            ble_gap_conn_desc description{};
            int result = ble_gap_conn_find(repeat.conn_handle, &description);
            if (result == 0) {
                result = ble_store_util_delete_peer(&description.peer_id_addr);
            }
            if (result != 0) {
                ESP_LOGW(kTag, "could not replace current peer bond: %d", result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            ESP_LOGI(kTag, "replacing current peer bond");
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_MTU:
            if (event->mtu.conn_handle == self->connection) {
                const uint16_t mtu = event->mtu.value;
                self->update_state([mtu](BleSnapshot& value) { value.mtu = mtu; });
            }
            break;
        default:
            break;
        }
        return 0;
    }

    static void report_bond_store(int object, int result, int rollback, bool update)
    {
        if (result == 0) {
            ESP_LOGI(kTag, "bond store persisted existing update; object=%d", object);
        } else {
            ESP_LOGE(kTag, "bond store failed; object=%d rc=%d rollback=%d existing_update=%u",
                     object, result, rollback, static_cast<unsigned>(update));
        }
    }

    static int write_bond_store(int object, const ble_store_value* value)
    {
        Impl* self = active.load(std::memory_order_acquire);
        return self != nullptr ? self->bond_store.write(object, value) : BLE_HS_ESTORE_FAIL;
    }

    bool enforce_store_health(uint16_t handle = BLE_HS_CONN_HANDLE_NONE)
    {
        const int error = bond_store.error();
        if (error == 0) return false;
        // IDF's SMP completion ignores store-write errors. Shut down radio
        // activity outside its store callback / host lock, and reject GATT
        // access immediately until the transport is fully reinitialized.
        if (handle != BLE_HS_CONN_HANDLE_NONE &&
            handle != connection && handle != peer_connection) {
            const int result = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            if (result != 0 && result != BLE_HS_ENOTCONN) {
                ESP_LOGW(kTag, "untracked link shutdown failed: %d", result);
            }
        }
        change_enabled(false);
        clear_response();
        update_state([](BleSnapshot& value) {
            value.encrypted = false;
            value.bonded = false;
            value.subscribed = false;
        });
        update_diagnostics([error](BleDiagnostics& value) { value.peer_error = error; });
        return true;
    }

    static int store_status(ble_store_status_event* event, void*)
    {
        switch (event->event_code) {
        case BLE_STORE_EVENT_FULL:
            // A replacement may still fit; let the store attempt it.
            return 0;
        case BLE_STORE_EVENT_OVERFLOW:
#if MYNEWT_VAL(BLE_HS_PVCY)
            if (event->overflow.obj_type == BLE_STORE_OBJ_TYPE_LOCAL_IRK &&
                event->overflow.value != nullptr) {
                // IDF allows one local IRK. An old local identity can occupy
                // that slot even when there are no peer bonds. Only retire a
                // record proven to belong to a different local identity.
                const auto& incoming = event->overflow.value->local_irk;
                uint8_t public_address[BLE_DEV_ADDR_LEN]{};
                int result = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr);
                if (result != 0 ||
                    incoming.addr.type != BLE_ADDR_PUBLIC ||
                    std::memcmp(incoming.addr.val, public_address, BLE_DEV_ADDR_LEN) != 0) {
                    ESP_LOGW(kTag, "local IRK overflow: incoming identity not verified");
                    return BLE_HS_ESTORE_CAP;
                }
                ble_store_key_local_irk key{};
                // Zero-initialized key.addr is BLE_ADDR_ANY without C compound literals.
                ble_store_value_local_irk previous{};
                result = ble_store_read_local_irk(&key, &previous);
                if (result != 0 || ble_addr_cmp(&previous.addr, &incoming.addr) == 0) {
                    ESP_LOGW(kTag, "local IRK overflow: matching identity retained (rc=%d)",
                             result);
                    return BLE_HS_ESTORE_CAP;
                }
                key.addr = previous.addr;
                result = ble_store_delete_local_irk(&key);
                if (result != 0) {
                    ESP_LOGW(kTag, "obsolete local IRK removal failed: %d", result);
                    return result;
                }
                ESP_LOGI(kTag, "retired obsolete local identity record; peer bonds retained");
                // The stack now retries its pending write of the current IRK.
                return 0;
            }
#endif
            // A new peer must not silently evict an existing bond. Repeat
            // pairing of the current peer is handled explicitly above.
            ESP_LOGW(kTag, "BLE store overflow for object=%d; records retained",
                     event->overflow.obj_type);
            return BLE_HS_ESTORE_CAP;
        default:
            // GEN_KEY uses store_gen_key_cb, not this status callback.
            ESP_LOGW(kTag, "unsupported BLE store event=%d", event->event_code);
            return BLE_HS_ENOTSUP;
        }
    }
    void notify_ready(uint16_t handle)
    {
        if (!snapshot().subscribed) {
            return;
        }
        // This is <= 13 bytes even for a 512-byte response and therefore fits
        // the default ATT MTU of 23. TX always retains the complete JSON; a
        // dropped notification can be recovered by reading TX after the write.
        char notification[20]{};
        const int length = std::snprintf(notification, sizeof(notification),
                                         "{\"ready\":%u}",
                                         static_cast<unsigned>(response_length));
        os_mbuf* packet = ble_hs_mbuf_from_flat(notification, length);
        if (packet == nullptr) {
            update_state([](BleSnapshot& value) { ++value.dropped; });
            return;
        }
        // NimBLE consumes packet on both success and error.
        const int result = ble_gatts_notify_custom(handle, tx_handle, packet);
        if (result != 0) {
            update_state([](BleSnapshot& value) { ++value.dropped; });
            ESP_LOGD(kTag, "ready notification unavailable: %d", result);
        }
    }

    static int access(uint16_t handle, uint16_t, ble_gatt_access_ctxt* context,
                      void* argument)
    {
        auto* self = static_cast<Impl*>(argument);
        if (self->bond_store.error() != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
        if (!self->adopt_connection(handle)) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        // Bond restoration does not always produce a fresh ENC_CHANGE event
        // on every client path. Read the live link state for each GATT access
        // so telemetry cannot retain a stale unencrypted snapshot.
        ble_gap_conn_desc connection_state{};
        if (ble_gap_conn_find(handle, &connection_state) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        const bool encrypted = connection_state.sec_state.encrypted != 0;
        const bool bonded = connection_state.sec_state.bonded != 0;
        self->update_state([encrypted, bonded](BleSnapshot& value) {
            value.encrypted = encrypted;
            value.bonded = bonded;
        });
        if (context->op == BLE_GATT_ACCESS_OP_READ_CHR &&
            ble_uuid_cmp(context->chr->uuid, &kTxUuid.u) == 0) {
            // NimBLE applies Read Blob offsets to this complete value. The
            // client must finish read/read-long before issuing its next write.
            return os_mbuf_append(context->om, self->response, self->response_length) == 0
                ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
            ble_uuid_cmp(context->chr->uuid, &kRxUuid.u) != 0) {
            return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        }
        const size_t length = OS_MBUF_PKTLEN(context->om);
        const uint16_t mtu = ble_att_mtu(handle);
        if (length == 0 || length > kMaxRequestBytes || mtu < 3 || length > mtu - 3U) {
            self->update_state([](BleSnapshot& value) { ++value.dropped; });
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        char request[kMaxRequestBytes + 1]{};
        if (os_mbuf_copydata(context->om, 0, length, request) != 0 ||
            std::memchr(request, '\0', length) != nullptr) {
            self->update_state([](BleSnapshot& value) { ++value.dropped; });
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        self->update_state([](BleSnapshot& value) { ++value.received; });
        self->handler(self->handler_context, request, length,
                      self->response, sizeof(self->response));
        // Do not turn an unterminated handler result into a truncated JSON frame.
        const size_t output_length = strnlen(self->response, sizeof(self->response));
        if (output_length == sizeof(self->response) || output_length == 0) {
            std::strcpy(self->response, "{\"error\":\"response_too_large\"}");
        }
        self->response_length = std::strlen(self->response);
        self->notify_ready(handle);
        return 0;
    }

    static std::atomic<Impl*> active;
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
    BleSnapshot state{};
    BleDiagnostics analyzer{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> radio_enabled{true};
    std::atomic<bool> host_synced{false};
    detail::BleBondStore bond_store{ble_store_config_read, ble_store_config_write,
                                    ble_store_config_delete, report_bond_store};
    uint16_t peer_connection = BLE_HS_CONN_HANDLE_NONE;
    bool peer_connect_allowed = false;
    bool peer_disconnect_requested = false; // Host task owns connection intent.
    ble_addr_t seen_addresses[kTrackedAddresses]{};
    size_t seen_count = 0;
    StaticQueue_t command_control{};
    alignas(Command) uint8_t command_storage[kCommandCapacity * sizeof(Command)]{};
    QueueHandle_t commands = nullptr;
    ble_npl_event command_event{};
    bool command_event_initialized = false;
    std::atomic<bool> host_event_pending{false};
    int64_t next_poll_us = 0;
    bool initialized = false;
    bool host_stop_requested = false;
    esp_err_t deinit_error = ESP_OK;
    RequestHandler handler = nullptr;
    void* handler_context = nullptr;
    char name[kMaxNameBytes + 1]{};
    uint8_t address_type = 0;
    uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
    uint16_t tx_handle = 0;
    char response[kResponseBytes + 1]{};
    size_t response_length = 0;
    ble_gatt_chr_def characteristics[3]{};
    ble_gatt_svc_def services[2]{};
};

std::atomic<BleTransport::Impl*> BleTransport::Impl::active{nullptr};

BleTransport::BleTransport() = default;

BleTransport::~BleTransport()
{
    Impl* self = impl_.load(std::memory_order_acquire);
    esp_err_t result = stop();
    // A failed stop must never destroy a live callback's object or handler
    // context. Cooperative task exit takes precedence over destructor latency.
    while (result != ESP_OK && self != nullptr && self->is_running()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        result = stop();
    }
    if (result == ESP_OK) {
        delete self;
    } else {
        // The host has exited but the controller/port did not deinitialize.
        // Preserve the stack-owned GATT definitions and singleton reservation.
        ESP_LOGE(kTag, "retaining BLE resources after deinit failure: %s",
                 esp_err_to_name(result));
    }
}

esp_err_t BleTransport::start(const char* name, RequestHandler handler, void* context)
{
#if CONFIG_BT_NIMBLE_SM_LEGACY || !CONFIG_BT_NIMBLE_SM_SC || !CONFIG_BT_NIMBLE_NVS_PERSIST
    ESP_LOGE(kTag, "BLE requires Secure Connections, legacy pairing disabled, and NVS bonds");
    return ESP_ERR_INVALID_STATE;
#endif
    Impl* self = impl_.load(std::memory_order_acquire);
    if (name == nullptr || handler == nullptr || name[0] == '\0' ||
        strnlen(name, kMaxNameBytes + 1) > kMaxNameBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (self == nullptr) {
        self = new (std::nothrow) Impl();
        if (self == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        impl_.store(self, std::memory_order_release);
    }
    if (self->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    Impl* expected = nullptr;
    if (!Impl::active.compare_exchange_strong(expected, self)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        Impl::active.store(nullptr, std::memory_order_release);
        ESP_LOGE(kTag, "NimBLE initialization failed: %s", esp_err_to_name(result));
        return result;
    }
    self->initialized = true;
    self->host_stop_requested = false;
    self->deinit_error = ESP_OK;
    self->stopping.store(false, std::memory_order_release);
    self->radio_enabled.store(true, std::memory_order_release);
    self->bond_store.reset();
    self->host_synced.store(false, std::memory_order_release);
    self->peer_connection = BLE_HS_CONN_HANDLE_NONE;
    self->peer_connect_allowed = false;
    self->peer_disconnect_requested = false;
    self->seen_count = 0;
    self->next_poll_us = 0;
    xQueueReset(self->commands);
    ble_npl_event_init(&self->command_event, Impl::process_host_event, self);
    self->command_event_initialized = true;
    self->host_event_pending.store(false, std::memory_order_release);
    self->update_diagnostics([](BleDiagnostics& value) {
        value = BleDiagnostics{};
        value.enabled = true;
    });
    self->handler = handler;
    self->handler_context = context;
    std::strcpy(self->name, name);
    self->clear_response();
    self->update_state([](BleSnapshot& value) { value = BleSnapshot{}; });
    ble_hs_cfg.reset_cb = Impl::on_reset;
    ble_hs_cfg.sync_cb = Impl::on_sync;
    // Secure Connections Just Works encrypts the link without MITM proof.
    // Disable legacy pairing in sdkconfig to require SC. IDF's sm_sc_only
    // additionally requires authenticated Level 4 GATT access, which would
    // incorrectly reject every Just Works peer with ATT error 0x05.
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_sec_lvl = 2;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = Impl::store_status;
#if !MYNEWT_VAL(BLE_STATIC_TO_DYNAMIC) || !MYNEWT_VAL(BLE_HS_PVCY)
    ble_store_config_init();
#else
    // In IDF's dynamic host, privacy startup initializes and restores the
    // store after the controller's public identity has become available.
#endif
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int status = ble_svc_gap_device_name_set(self->name);
    if (status == 0) {
        status = ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);
    }
    if (status == 0) {
        status = ble_gatts_count_cfg(self->services);
    }
    if (status == 0) {
        status = ble_gatts_add_svcs(self->services);
    }
    if (status != 0) {
        ESP_LOGE(kTag, "GATT initialization failed: %d", status);
        stop();
        return ESP_FAIL;
    }
    self->update_state([](BleSnapshot& value) { value.enabled = true; });
    if (!self->AppTask::start()) {
        ESP_LOGE(kTag, "could not create NimBLE host task");
        stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "BLE host started; name=%s", self->name);
    return ESP_OK;
}

esp_err_t BleTransport::stop()
{
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr || !self->initialized) {
        return ESP_OK;
    }
    self->stopping.store(true, std::memory_order_release);
    self->host_synced.store(false, std::memory_order_release);
    self->radio_enabled.store(false, std::memory_order_release);
    if (self->is_running() && !self->host_stop_requested) {
        // This signals the host, disconnects peers, stops advertising, and
        // wakes nimble_port_run. It must be called outside the host task.
        const int result = nimble_port_stop();
        if (result != 0) {
            ESP_LOGE(kTag, "NimBLE host stop failed: %d", result);
            return ESP_FAIL;
        }
        self->host_stop_requested = true;
    }
    if (!self->AppTask::stop(pdMS_TO_TICKS(2000))) {
        return ESP_ERR_TIMEOUT;
    }
    // No application callback is running beyond this point.
    if (self->command_event_initialized) {
        ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &self->command_event);
        ble_npl_event_deinit(&self->command_event);
        self->command_event_initialized = false;
    }
    self->host_event_pending.store(false, std::memory_order_release);
    xQueueReset(self->commands);
    self->peer_connection = BLE_HS_CONN_HANDLE_NONE;
    self->peer_connect_allowed = false;
    self->peer_disconnect_requested = false;
    self->update_diagnostics([](BleDiagnostics& value) {
        value.enabled = false;
        value.scanning = false;
        value.connecting = false;
        value.peer_connected = false;
        value.peer_mtu = 23;
        value.peer_rssi = 0;
    });
    self->connection = BLE_HS_CONN_HANDLE_NONE;
    self->clear_response();
    self->update_state([](BleSnapshot& value) {
        value.enabled = false;
        value.advertising = false;
        value.connected = false;
        value.subscribed = false;
        value.encrypted = false;
        value.bonded = false;
        value.mtu = 23;
    });
    self->handler = nullptr;
    self->handler_context = nullptr;
    if (self->deinit_error != ESP_OK) {
        // IDF deinitializes the host before disabling/deinitializing the
        // controller. A later failure must not repeat host destruction.
        return self->deinit_error;
    }
    self->deinit_error = nimble_port_deinit();
    if (self->deinit_error != ESP_OK) {
        ESP_LOGE(kTag, "NimBLE deinitialization failed: %s",
                 esp_err_to_name(self->deinit_error));
        return self->deinit_error;
    }
    self->initialized = false;

    Impl::active.store(nullptr, std::memory_order_release);
    return ESP_OK;
}


esp_err_t BleTransport::request_scan()
{
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr) return ESP_ERR_INVALID_STATE;
    Impl::Command command{};
    command.kind = Impl::CommandKind::scan;
    return self->enqueue(command);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t BleTransport::connect_peer(const char* address, uint8_t address_type)
{
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr) return ESP_ERR_INVALID_STATE;
    Impl::Command command{};
    command.kind = Impl::CommandKind::connect;
    if (!Impl::parse_address(address, address_type, command.address)) {
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[18]{};
    Impl::format_address(command.address, normalized);
    const BleDiagnostics observed = self->diagnostics();
    if (!observed.enabled || observed.peer_connected || observed.connecting) {
        return ESP_ERR_INVALID_STATE;
    }
    bool found = false;
    for (size_t index = 0; index < observed.count; ++index) {
        const auto& device = observed.devices[index];
        if (device.address_type == address_type &&
            std::strcmp(device.address, normalized) == 0 && device.connectable) {
            std::memcpy(command.peer_name, device.name, sizeof(command.peer_name));
            found = true;
            break;
        }
    }
    // Connections require an actual connectable result selected from the
    // latest scan. Never probe arbitrary addresses supplied by a remote client.
    return found ? self->enqueue(command) : ESP_ERR_NOT_FOUND;
#else
    (void)address;
    (void)address_type;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t BleTransport::disconnect_peer()
{
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr) return ESP_ERR_INVALID_STATE;
    Impl::Command command{};
    command.kind = Impl::CommandKind::disconnect;
    return self->enqueue(command);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t BleTransport::set_enabled(bool enabled)
{
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr) return ESP_ERR_INVALID_STATE;
    Impl::Command command{};
    command.kind = Impl::CommandKind::enable;
    command.enabled = enabled;
    return self->enqueue(command);
}

void BleTransport::process()
{
    Impl* self = impl_.load(std::memory_order_acquire);
    if (self == nullptr || !self->initialized ||
        self->stopping.load(std::memory_order_acquire) ||
        !self->host_synced.load(std::memory_order_acquire)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (uxQueueMessagesWaiting(self->commands) == 0 && now < self->next_poll_us) {
        return;
    }
    bool expected = false;
    if (!self->host_event_pending.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        return;
    }
    self->next_poll_us = now + 1000000;
    // One producer and one outstanding event avoid races on NPL's queued bit.
    // Its queue is sized to the event pool, so this reserved event has a slot.
    // All GAP/GATT calls and mutable connection handles stay on the host task.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &self->command_event);
}

BleDiagnostics BleTransport::diagnostics()
{
    Impl* self = impl_.load(std::memory_order_acquire);
    return self == nullptr ? BleDiagnostics{} : self->diagnostics();
}

BleSnapshot BleTransport::snapshot()
{
    Impl* self = impl_.load(std::memory_order_acquire);
    return self == nullptr ? BleSnapshot{} : self->snapshot();
}

} // namespace connectivity

#else

namespace connectivity {

BleTransport::BleTransport() = default;
BleTransport::~BleTransport() = default;

esp_err_t BleTransport::start(const char*, RequestHandler, void*)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t BleTransport::stop() { return ESP_OK; }
BleSnapshot BleTransport::snapshot() { return {}; }

esp_err_t BleTransport::request_scan() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t BleTransport::connect_peer(const char*, uint8_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t BleTransport::disconnect_peer() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t BleTransport::set_enabled(bool) { return ESP_ERR_NOT_SUPPORTED; }
void BleTransport::process() {}
BleDiagnostics BleTransport::diagnostics() { return {}; }


} // namespace connectivity

#endif
