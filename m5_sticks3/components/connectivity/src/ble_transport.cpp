#include "ble_transport.hpp"

#include "sdkconfig.h"

#if CONFIG_M5_CONNECTIVITY_BLE_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "app_task.hpp"
#include "connectivity_policy.hpp"
#include "esp_log.h"
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

    void clear_response()
    {
        // Never expose the previous peer's command response to a new peer.
        std::memset(response, 0, sizeof(response));
        std::strcpy(response, "{\"error\":\"no_response\"}");
        response_length = std::strlen(response);
    }

    void advertise()
    {
        if (stopping.load(std::memory_order_acquire)) {
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
        update_state([result](BleSnapshot& value) { value.advertising = result == 0; });
        if (result != 0) {
            ESP_LOGE(kTag, "advertising failed: %d", result);
        }
    }

    static void on_sync()
    {
        Impl* self = active.load(std::memory_order_acquire);
        if (self == nullptr || self->stopping.load(std::memory_order_acquire)) {
            return;
        }
        int result = ble_hs_util_ensure_addr(0);
        if (result == 0) {
            result = ble_hs_id_infer_auto(0, &self->address_type);
        }
        if (result != 0) {
            ESP_LOGE(kTag, "address initialization failed: %d", result);
            return;
        }
        self->advertise();
    }

    static void on_reset(int reason)
    {
        Impl* self = active.load(std::memory_order_acquire);
        if (self == nullptr) {
            return;
        }
        self->connection = BLE_HS_CONN_HANDLE_NONE;
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
        if (stopping.load(std::memory_order_acquire)) {
            return false;
        }
        if (connection == handle) {
            return true;
        }
        if (connection != BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        ble_gap_conn_desc description{};
        if (ble_gap_conn_find(handle, &description) != 0) {
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
            if (self->stopping.load(std::memory_order_acquire) ||
                repeat.conn_handle != self->connection || !repeat.new_sc ||
                repeat.new_key_size != 16 || !repeat.new_bonding ||
                (repeat.cur_authenticated && !repeat.new_authenticated)) {
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
    std::atomic<bool> stopping{false};
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

} // namespace connectivity

#endif
