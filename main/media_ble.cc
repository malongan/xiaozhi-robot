#include "media_ble.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#define TAG "MediaBle"

extern "C" void ble_store_config_init(void);

namespace {

constexpr const char* kDeviceName = "Xiaozhi-Media";

const ble_uuid16_t kHidServiceUuid = BLE_UUID16_INIT(0x1812);
const ble_uuid16_t kHidInfoUuid = BLE_UUID16_INIT(0x2A4A);
const ble_uuid16_t kReportMapUuid = BLE_UUID16_INIT(0x2A4B);
const ble_uuid16_t kHidControlUuid = BLE_UUID16_INIT(0x2A4C);
const ble_uuid16_t kReportUuid = BLE_UUID16_INIT(0x2A4D);
const ble_uuid16_t kProtocolModeUuid = BLE_UUID16_INIT(0x2A4E);
const ble_uuid16_t kReportRefUuid = BLE_UUID16_INIT(0x2908);
const ble_uuid16_t kDisServiceUuid = BLE_UUID16_INIT(0x180A);
const ble_uuid16_t kManufacturerUuid = BLE_UUID16_INIT(0x2A29);
const ble_uuid16_t kPnpIdUuid = BLE_UUID16_INIT(0x2A50);
const ble_uuid16_t kBatteryServiceUuid = BLE_UUID16_INIT(0x180F);
const ble_uuid16_t kBatteryLevelUuid = BLE_UUID16_INIT(0x2A19);

// One combined input report (ID 1): keyboard (8 bytes) + nested consumer keys
// (1 byte).  A single report characteristic binds reliably on macOS; multiple
// 0x2A4D report characteristics were ignored by its HID stack.
const uint8_t kReportMap[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (Left Control)
    0x29, 0xE7,  //   Usage Maximum (Right GUI)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data,Var,Abs) -> modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Const) -> reserved byte
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data,Array) -> 6 keycodes

    0x05, 0x0C,                                      //   Usage Page (Consumer)
    0x09, 0x01,                                      //   Usage (Consumer Control)
    0xA1, 0x01,                                      //   Collection (Logical)
    0x15, 0x00,                                      //     Logical Minimum (0)
    0x25, 0x01,                                      //     Logical Maximum (1)
    0x75, 0x01,                                      //     Report Size (1)
    0x95, 0x10,                                      //     Report Count (16)
    0x09, 0xB5,                                      //     Usage (Scan Next Track)
    0x09, 0xB6,                                      //     Usage (Scan Previous Track)
    0x09, 0xCD,                                      //     Usage (Play/Pause)
    0x09, 0xE2,                                      //     Usage (Mute)
    0x09, 0xE9,                                      //     Usage (Volume Increment)
    0x09, 0xEA,                                      //     Usage (Volume Decrement)
    0x09, 0x70,                                      //     Usage (Brightness Decrement)
    0x09, 0x6F,                                      //     Usage (Brightness Increment)
    0x09, 0x00, 0x09, 0x00, 0x09, 0x00, 0x09, 0x00,  // padding bits
    0x09, 0x00, 0x09, 0x00, 0x09, 0x00, 0x09, 0x00,
    0x81, 0x02,  //     Input (Data,Var,Abs) -> 2 consumer bytes
    0xC0,        //   End Collection (Logical)
    0xC0,        // End Collection (Application)
};

const uint8_t kHidInfo[] = {0x11, 0x01, 0x00, 0x02};  // bcdHID 1.11, flags
const uint8_t kReportRefInput[] = {0x01, 0x01};       // report id 1, input
const uint8_t kManufacturerName[] = "Xiaozhi";
const uint8_t kPnpId[] = {0x02, 0xE5, 0x02, 0x01, 0x00, 0x00, 0x01};
uint8_t g_protocol_mode = 1;      // report protocol
uint8_t g_report_value[10] = {};  // 8 keyboard bytes + 2 consumer bytes
uint8_t g_battery_level = 100;

uint8_t g_own_addr_type = 0;

ble_gatt_chr_def g_hid_chrs[6] = {};
ble_gatt_dsc_def g_hid_dscs[2] = {};
ble_gatt_svc_def g_svcs[4] = {};
ble_gatt_chr_def g_di_chrs[3] = {};
ble_gatt_chr_def g_batt_chrs[2] = {};

int HidInfoAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    return os_mbuf_append(ctxt->om, kHidInfo, sizeof(kHidInfo)) == 0 ? 0
                                                                     : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int ReportMapAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    ESP_LOGI(TAG, "host read report map (%d bytes)", static_cast<int>(sizeof(kReportMap)));
    return os_mbuf_append(ctxt->om, kReportMap, sizeof(kReportMap)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int ControlPointAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    return ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR ? 0 : BLE_ATT_ERR_UNLIKELY;
}

int ProtocolModeAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &g_protocol_mode, 1) == 0 ? 0
                                                                  : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && OS_MBUF_PKTLEN(ctxt->om) >= 1) {
        if (os_mbuf_copydata(ctxt->om, 0, 1, &g_protocol_mode) == 0) {
            ESP_LOGI(TAG, "protocol mode <- %d", g_protocol_mode);
            return 0;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

int InputReportAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "host read input report (%d bytes)",
                 static_cast<int>(sizeof(g_report_value)));
        return os_mbuf_append(ctxt->om, g_report_value, sizeof(g_report_value)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

int InputReportRefAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    return os_mbuf_append(ctxt->om, kReportRefInput, sizeof(kReportRefInput)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int ManufacturerAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    return os_mbuf_append(ctxt->om, kManufacturerName, sizeof(kManufacturerName) - 1) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int PnpIdAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    return os_mbuf_append(ctxt->om, kPnpId, sizeof(kPnpId)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int BatteryLevelAccess(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &g_battery_level, 1) == 0 ? 0
                                                                  : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

}  // namespace

MediaBle& MediaBle::GetInstance() {
    static MediaBle instance;
    return instance;
}

void MediaBle::SetStatusCallback(StatusCallback callback) {
    status_callback_ = std::move(callback);
}

void MediaBle::InitGattServices() {
    g_hid_dscs[0] = {};
    g_hid_dscs[0].uuid = &kReportRefUuid.u;
    g_hid_dscs[0].att_flags = BLE_ATT_F_READ;
    g_hid_dscs[0].access_cb = InputReportRefAccess;
    g_hid_dscs[1] = {};

    g_hid_chrs[0] = {};
    g_hid_chrs[0].uuid = &kHidInfoUuid.u;
    g_hid_chrs[0].access_cb = HidInfoAccess;
    g_hid_chrs[0].flags = BLE_GATT_CHR_F_READ;
    g_hid_chrs[1] = {};
    g_hid_chrs[1].uuid = &kReportMapUuid.u;
    g_hid_chrs[1].access_cb = ReportMapAccess;
    g_hid_chrs[1].flags = BLE_GATT_CHR_F_READ;
    g_hid_chrs[2] = {};
    g_hid_chrs[2].uuid = &kHidControlUuid.u;
    g_hid_chrs[2].access_cb = ControlPointAccess;
    g_hid_chrs[2].flags = BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_hid_chrs[3] = {};
    g_hid_chrs[3].uuid = &kProtocolModeUuid.u;
    g_hid_chrs[3].access_cb = ProtocolModeAccess;
    g_hid_chrs[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_hid_chrs[4] = {};
    g_hid_chrs[4].uuid = &kReportUuid.u;
    g_hid_chrs[4].access_cb = InputReportAccess;
    g_hid_chrs[4].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC |
                          BLE_GATT_CHR_F_WRITE_ENC;
    g_hid_chrs[4].descriptors = g_hid_dscs;
    g_hid_chrs[5] = {};

    g_di_chrs[0] = {};
    g_di_chrs[0].uuid = &kManufacturerUuid.u;
    g_di_chrs[0].access_cb = ManufacturerAccess;
    g_di_chrs[0].flags = BLE_GATT_CHR_F_READ;
    g_di_chrs[1] = {};
    g_di_chrs[1].uuid = &kPnpIdUuid.u;
    g_di_chrs[1].access_cb = PnpIdAccess;
    g_di_chrs[1].flags = BLE_GATT_CHR_F_READ;
    g_di_chrs[2] = {};

    g_batt_chrs[0] = {};
    g_batt_chrs[0].uuid = &kBatteryLevelUuid.u;
    g_batt_chrs[0].access_cb = BatteryLevelAccess;
    g_batt_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    g_batt_chrs[1] = {};

    g_svcs[0] = {};
    g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[0].uuid = &kHidServiceUuid.u;
    g_svcs[0].characteristics = g_hid_chrs;
    g_svcs[1] = {};
    g_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[1].uuid = &kDisServiceUuid.u;
    g_svcs[1].characteristics = g_di_chrs;
    g_svcs[2] = {};
    g_svcs[2].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[2].uuid = &kBatteryServiceUuid.u;
    g_svcs[2].characteristics = g_batt_chrs;
    g_svcs[3] = {};

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(g_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(g_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
    }
}

void MediaBle::Start() {
    if (started_) {
        return;
    }
    started_ = true;

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return;
    }

    ble_hs_cfg.reset_cb = &MediaBle::OnReset;
    ble_hs_cfg.sync_cb = &MediaBle::OnSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    InitGattServices();
    ble_store_config_init();

    nimble_port_freertos_init(&MediaBle::HostTask);
    ESP_LOGI(TAG, "BLE HID started (single-report layout v2), advertising as %s", kDeviceName);
}

void MediaBle::HostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void MediaBle::OnReset(int reason) { ESP_LOGW(TAG, "host reset, reason=%d", reason); }

void MediaBle::OnSync() {
    ble_svc_gap_device_name_set(kDeviceName);
    if (ble_hs_id_infer_auto(0, &g_own_addr_type) != 0) {
        ESP_LOGE(TAG, "failed to infer address type");
    }
    auto& self = GetInstance();
    if (ble_gatts_find_chr(&kHidServiceUuid.u, &kReportUuid.u, nullptr, &self.report_val_handle_) !=
        0) {
        ESP_LOGE(TAG, "report characteristic not found");
    }
    self.StartAdvertising();
    ESP_LOGI(TAG, "host synced, advertise, report val handle=%d", self.report_val_handle_);
}

void MediaBle::StartAdvertising() {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = 0x03C1;  // keyboard
    fields.appearance_is_present = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {};
    rsp.name = reinterpret_cast<const uint8_t*>(kDeviceName);
    rsp.name_len = static_cast<uint8_t>(strlen(kDeviceName));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, &MediaBle::GapEvent,
                           nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

int MediaBle::GapEvent(struct ble_gap_event* event, void*) {
    auto& self = GetInstance();
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self.conn_handle_ = event->connect.conn_handle;
                self.connected_ = true;
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(self.conn_handle_, &desc) == 0) {
                    ESP_LOGI(TAG, "host connected, handle=%d peer=%02X:%02X:%02X:%02X:%02X:%02X",
                             self.conn_handle_, desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                             desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                             desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
                } else {
                    ESP_LOGI(TAG, "host connected, handle=%d", self.conn_handle_);
                }
                const int sec_rc = ble_gap_security_initiate(self.conn_handle_);
                ESP_LOGI(TAG, "security initiate rc=%d", sec_rc);
                if (self.status_callback_) {
                    self.status_callback_(true);
                }
            } else {
                self.StartAdvertising();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "host disconnected, reason=%d", event->disconnect.reason);
            self.connected_ = false;
            self.conn_handle_ = 0xFFFF;
            if (self.status_callback_) {
                self.status_callback_(false);
            }
            self.StartAdvertising();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self.StartAdvertising();
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                ESP_LOGW(TAG, "repeat pairing: dropped stale bond");
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption change, status=%d", event->enc_change.status);
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            self.subscribed_handle_ = event->subscribe.attr_handle;
            self.subscribed_notify_ = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "subscribe, handle=%d notify=%d report_val_handle=%d",
                     event->subscribe.attr_handle, event->subscribe.cur_notify,
                     self.report_val_handle_);
            return 0;
        default:
            return 0;
    }
}

int MediaBle::GetBondedPeers(MediaBlePeer* peers, int max_peers) {
    if (peers == nullptr || max_peers <= 0) {
        return 0;
    }
    ble_addr_t addrs[CONFIG_BT_NIMBLE_MAX_BONDS] = {};
    int count = 0;
    if (ble_store_util_bonded_peers(addrs, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) {
        return 0;
    }
    if (count > max_peers) {
        count = max_peers;
    }
    for (int i = 0; i < count; ++i) {
        peers[i].type = addrs[i].type;
        memcpy(peers[i].addr, addrs[i].val, 6);
    }
    return count;
}

bool MediaBle::Unpair(const MediaBlePeer& peer) {
    ble_addr_t addr = {};
    addr.type = peer.type;
    memcpy(addr.val, peer.addr, 6);
    return ble_gap_unpair(&addr) == 0;
}

bool MediaBle::GetConnectedPeer(MediaBlePeer* peer) {
    if (!connected_ || conn_handle_ == 0xFFFF || peer == nullptr) {
        return false;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle_, &desc) != 0) {
        return false;
    }
    peer->type = desc.peer_id_addr.type;
    memcpy(peer->addr, desc.peer_id_addr.val, 6);
    return true;
}

bool MediaBle::Disconnect() {
    if (!connected_ || conn_handle_ == 0xFFFF) {
        return false;
    }
    return ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM) == 0;
}

bool MediaBle::SendShortcut(uint8_t modifiers, uint8_t keycode) {
    if (!connected_ || conn_handle_ == 0xFFFF || report_val_handle_ == 0) {
        return false;
    }
    uint8_t pressed[sizeof(g_report_value)] = {};
    pressed[0] = modifiers;
    pressed[2] = keycode;
    uint8_t released[sizeof(g_report_value)] = {};
    auto send = [this](const uint8_t* data) {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(data, sizeof(g_report_value));
        if (om != nullptr) {
            return ble_gatts_notify_custom(conn_handle_, report_val_handle_, om);
        }
        return static_cast<int>(BLE_HS_ENOMEM);
    };
    int rc = send(pressed);
    vTaskDelay(pdMS_TO_TICKS(30));
    send(released);
    ESP_LOGI(TAG, "send shortcut mod=0x%02X key=0x%02X rc=%d", modifiers, keycode, rc);
    return true;
}

bool MediaBle::SendKey(MediaBleKey key) {
    if (!connected_ || conn_handle_ == 0xFFFF || report_val_handle_ == 0) {
        return false;
    }
    if (key == MediaBleKey::kLockScreen) {
        // Ctrl+Cmd+Q
        return SendShortcut(0x08 | 0x01, 0x14);
    }
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(key));
    int rc = 0;
    auto notify = [this, &rc](const uint8_t* data) {
        memcpy(g_report_value, data, sizeof(g_report_value));
        struct os_mbuf* om = ble_hs_mbuf_from_flat(data, sizeof(g_report_value));
        if (om == nullptr) {
            rc = BLE_HS_ENOMEM;
            return;
        }
        rc = ble_gatts_notify_custom(conn_handle_, report_val_handle_, om);
    };
    uint8_t pressed[sizeof(g_report_value)] = {};
    const int consumer_index = 8 + (static_cast<int>(key) >= 8 ? 1 : 0);
    pressed[consumer_index] |= static_cast<uint8_t>(
        1u << (static_cast<uint8_t>(key) & 7u));  // consumer bits follow keyboard
    uint8_t released[sizeof(g_report_value)] = {};
    notify(pressed);
    vTaskDelay(pdMS_TO_TICKS(20));
    notify(released);
    ESP_LOGI(TAG, "send key %d bit=0x%02X val_handle=%d rc=%d", static_cast<int>(key),
             pressed[consumer_index], report_val_handle_, rc);
    return true;
}
