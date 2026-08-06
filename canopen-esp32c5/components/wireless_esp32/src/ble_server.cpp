#include "wireless_esp32/ble_server.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <algorithm>
#include <cstring>

extern "C" void ble_store_config_init(void);

namespace wireless_esp32 {
namespace {
constexpr char kTag[] = "wireless_ble";
constexpr uint16_t kNoConnection = 0xFFFFU;

const ble_uuid128_t kServiceUuid =
    BLE_UUID128_INIT(0x01, 0xC0, 0xC5, 0x5A, 0xA5, 0x4D, 0x4F, 0x54,
                     0x4F, 0x52, 0x47, 0x41, 0x54, 0x45, 0x57, 0x41);
const ble_uuid128_t kRxUuid =
    BLE_UUID128_INIT(0x02, 0xC0, 0xC5, 0x5A, 0xA5, 0x4D, 0x4F, 0x54,
                     0x4F, 0x52, 0x47, 0x41, 0x54, 0x45, 0x57, 0x41);
const ble_uuid128_t kTxUuid =
    BLE_UUID128_INIT(0x03, 0xC0, 0xC5, 0x5A, 0xA5, 0x4D, 0x4F, 0x54,
                     0x4F, 0x52, 0x47, 0x41, 0x54, 0x45, 0x57, 0x41);

} // namespace

BleServer* BleServer::instance_ = nullptr;

BleServer::BleServer(BleServerConfig config,
                     const SettingsStore& settings_store,
                     PacketSink& packet_sink)
    : config_(config), settings_store_(settings_store), packet_sink_(packet_sink)
{
}

BleServer::~BleServer()
{
    (void)deinitialize();
}

bool BleServer::initialize()
{
    if (initialized_.load(std::memory_order_acquire) || instance_ != nullptr ||
        !settings_store_.ready()) {
        return false;
    }
    instance_ = this;
    if (nimble_port_init() != ESP_OK) {
        instance_ = nullptr;
        return false;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_sm_configure_static_passkey(settings_store_.settings().ble_passkey, true);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    characteristics_[0].uuid = &kRxUuid.u;
    characteristics_[0].access_cb = gatt_access;
    characteristics_[0].arg = this;
    characteristics_[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                                BLE_GATT_CHR_F_WRITE_ENC |
                                BLE_GATT_CHR_F_WRITE_AUTHEN;
    characteristics_[0].min_key_size = 16;
    characteristics_[0].val_handle = &rx_value_handle_;
    characteristics_[1].uuid = &kTxUuid.u;
    characteristics_[1].access_cb = gatt_access;
    characteristics_[1].arg = this;
    characteristics_[1].flags = BLE_GATT_CHR_F_NOTIFY;
    characteristics_[1].val_handle = &tx_value_handle_;

    services_[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services_[0].uuid = &kServiceUuid.u;
    services_[0].characteristics = characteristics_.data();

    if (ble_gatts_count_cfg(services_.data()) != 0 ||
        ble_gatts_add_svcs(services_.data()) != 0 ||
        ble_svc_gap_device_name_set(config_.device_name.data()) != 0) {
        (void)nimble_port_deinit();
        instance_ = nullptr;
        return false;
    }

    ble_store_config_init();
    initialized_.store(true, std::memory_order_release);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(kTag, "BLE secure GATT initialized; device=%s", config_.device_name.data());
    return true;
}

bool BleServer::deinitialize()
{
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    subscribed_.store(false, std::memory_order_relaxed);
    const uint16_t connection = connection_handle_.exchange(
        kNoConnection, std::memory_order_acq_rel);
    if (connection != kNoConnection) {
        (void)ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
    }
    (void)ble_gap_adv_stop();
    const int stop_result = nimble_port_stop();
    const int deinit_result = stop_result == 0 ? nimble_port_deinit() : stop_result;
    instance_ = nullptr;
    return stop_result == 0 && deinit_result == 0;
}

bool BleServer::notify(const wireless::Packet& packet)
{
    const uint16_t connection = connection_handle_.load(std::memory_order_acquire);
    if (!initialized_.load(std::memory_order_acquire) || connection == kNoConnection ||
        !subscribed_.load(std::memory_order_acquire)) {
        return false;
    }
    ble_gap_conn_desc description{};
    if (ble_gap_conn_find(connection, &description) != 0 ||
        !description.sec_state.encrypted || !description.sec_state.authenticated) {
        return false;
    }

    wireless::WirePacket wire{};
    if (!wireless::encode_packet(packet, wire)) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    os_mbuf* buffer = ble_hs_mbuf_from_flat(wire.bytes.data(), wire.size);
    if (buffer == nullptr ||
        ble_gatts_notify_custom(connection, tx_value_handle_, buffer) != 0) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    tx_packets_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

BleServerStatistics BleServer::statistics() const
{
    return {
        .connections = connections_.load(std::memory_order_relaxed),
        .rx_packets = rx_packets_.load(std::memory_order_relaxed),
        .rx_invalid = rx_invalid_.load(std::memory_order_relaxed),
        .tx_packets = tx_packets_.load(std::memory_order_relaxed),
        .tx_dropped = tx_dropped_.load(std::memory_order_relaxed),
    };
}

int BleServer::gap_event(ble_gap_event* event, void* argument)
{
    auto* self = static_cast<BleServer*>(argument);
    return (self != nullptr ? self : instance_)->handle_gap_event(event);
}

int BleServer::gatt_access(uint16_t connection_handle,
                           uint16_t attribute_handle,
                           ble_gatt_access_ctxt* context,
                           void* argument)
{
    return static_cast<BleServer*>(argument)->handle_gatt_access(
        connection_handle, attribute_handle, context);
}

void BleServer::on_sync()
{
    if (instance_ == nullptr) {
        return;
    }
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &instance_->own_address_type_) != 0) {
        ESP_LOGE(kTag, "could not determine BLE address");
        return;
    }
    instance_->advertise();
}

void BleServer::on_reset(int reason)
{
    ESP_LOGE(kTag, "NimBLE host reset: reason=%d", reason);
}

void BleServer::host_task(void*)
{
    ESP_LOGI(kTag, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int BleServer::handle_gap_event(ble_gap_event* event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection_handle_.store(event->connect.conn_handle, std::memory_order_release);
            connections_.fetch_add(1, std::memory_order_relaxed);
            subscribed_.store(false, std::memory_order_relaxed);
            const ble_gap_upd_params parameters{
                .itvl_min = 12,
                .itvl_max = 24,
                .latency = 0,
                .supervision_timeout = 400,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            (void)ble_gap_update_params(event->connect.conn_handle, &parameters);
            (void)ble_gap_security_initiate(event->connect.conn_handle);
            ESP_LOGI(kTag, "BLE client connected; secure pairing requested");
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        connection_handle_.store(kNoConnection, std::memory_order_release);
        subscribed_.store(false, std::memory_order_release);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_value_handle_) {
            subscribed_.store(event->subscribe.cur_notify != 0, std::memory_order_release);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ble_gap_conn_desc description{};
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &description) == 0) {
            ble_store_util_delete_peer(&description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            ble_sm_io input{};
            input.action = BLE_SM_IOACT_DISP;
            input.passkey = settings_store_.settings().ble_passkey;
            return ble_sm_inject_io(event->passkey.conn_handle, &input);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(kTag,
                 "BLE encryption changed: status=%d handle=%u",
                 event->enc_change.status,
                 event->enc_change.conn_handle);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(kTag, "BLE MTU negotiated: %u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

int BleServer::handle_gatt_access(uint16_t connection_handle,
                                  uint16_t attribute_handle,
                                  ble_gatt_access_ctxt* context)
{
    if (attribute_handle != rx_value_handle_ ||
        context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    std::array<uint8_t, wireless::kMaxPacketSize> bytes{};
    uint16_t length = 0;
    if (ble_hs_mbuf_to_flat(context->om, bytes.data(), bytes.size(), &length) != 0) {
        rx_invalid_.fetch_add(1, std::memory_order_relaxed);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    wireless::Packet packet{};
    const wireless::DecodeResult decoded =
        wireless::decode_packet(std::span<const uint8_t>(bytes.data(), length), packet);
    if (decoded.status != wireless::DecodeStatus::ok || decoded.consumed != length) {
        rx_invalid_.fetch_add(1, std::memory_order_relaxed);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (!packet_sink_.submit(
            {.link = LinkKind::ble,
             .peer = static_cast<uint8_t>(connection_handle & 0xFFU),
             .packet = packet})) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    rx_packets_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void BleServer::advertise()
{
    ble_hs_adv_fields advertising{};
    advertising.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertising.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid);
    advertising.num_uuids128 = 1;
    advertising.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&advertising) != 0) {
        ESP_LOGE(kTag, "could not set BLE advertising data");
        return;
    }

    ble_hs_adv_fields scan_response{};
    const char* name = ble_svc_gap_device_name();
    scan_response.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name));
    scan_response.name_len = std::strlen(name);
    scan_response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan_response) != 0) {
        ESP_LOGE(kTag, "could not set BLE scan response");
        return;
    }

    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int result = ble_gap_adv_start(own_address_type_,
                                         nullptr,
                                         BLE_HS_FOREVER,
                                         &parameters,
                                         gap_event,
                                         this);
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGE(kTag, "BLE advertising failed: rc=%d", result);
    }
}

} // namespace wireless_esp32
