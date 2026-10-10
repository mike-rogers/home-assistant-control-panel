#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "improv_ble.h"

static const char *TAG = "improv";

// Improv state machine (spec values)
#define IMPROV_STATE_AUTHORIZED 0x02
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED 0x04

#define IMPROV_ERR_NONE 0x00
#define IMPROV_ERR_INVALID_RPC 0x01
#define IMPROV_ERR_UNKNOWN_CMD 0x02
#define IMPROV_ERR_UNABLE_TO_CONNECT 0x03

#define IMPROV_CMD_WIFI_SETTINGS 0x01

// 00467768-6228-2272-4663-2774782680XX, NimBLE wants little-endian bytes.
#define IMPROV_UUID128(last)                                              \
    BLE_UUID128_INIT(last, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,      \
                     0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00)

static const ble_uuid128_t UUID_SVC = IMPROV_UUID128(0x00);
static const ble_uuid128_t UUID_STATE = IMPROV_UUID128(0x01);
static const ble_uuid128_t UUID_ERROR = IMPROV_UUID128(0x02);
static const ble_uuid128_t UUID_RPC_CMD = IMPROV_UUID128(0x03);
static const ble_uuid128_t UUID_RPC_RESULT = IMPROV_UUID128(0x04);
static const ble_uuid128_t UUID_CAPABILITIES = IMPROV_UUID128(0x05);

static improv_connect_cb_t s_connect_cb;
static const char *s_device_name;
static SemaphoreHandle_t s_provisioned;

static uint8_t s_state = IMPROV_STATE_AUTHORIZED;
static uint8_t s_error = IMPROV_ERR_NONE;
static uint8_t s_rpc_result[80];
static size_t s_rpc_result_len;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_state, s_h_error, s_h_rpc_result;
static uint8_t s_own_addr_type;

static char s_pending_ssid[33];
static char s_pending_pass[65];
static volatile bool s_stopping;

static void advertise(void);

static void notify(uint16_t attr_handle, const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
    }
}

static void set_state(uint8_t state)
{
    s_state = state;
    notify(s_h_state, &s_state, 1);
}

static void set_error(uint8_t error)
{
    s_error = error;
    notify(s_h_error, &s_error, 1);
}

static uint8_t checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

// Worker task: try the credentials outside the NimBLE host task.
static void provision_task(void *arg)
{
    bool ok = s_connect_cb(s_pending_ssid, s_pending_pass);
    if (!ok) {
        set_error(IMPROV_ERR_UNABLE_TO_CONNECT);
        set_state(IMPROV_STATE_AUTHORIZED);
        vTaskDelete(NULL);
        return;
    }

    // Build the RPC result: the device URL, so the app can offer a link.
    char url[64] = "";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(url, sizeof(url), "http://" IPSTR "/api/info",
                 IP2STR(&ip_info.ip));
    }
    size_t url_len = strlen(url);
    uint8_t *out = s_rpc_result;
    out[0] = IMPROV_CMD_WIFI_SETTINGS;
    out[1] = (uint8_t)(url_len + 1);
    out[2] = (uint8_t)url_len;
    memcpy(&out[3], url, url_len);
    out[3 + url_len] = checksum(out, 3 + url_len);
    s_rpc_result_len = 4 + url_len;

    set_error(IMPROV_ERR_NONE);
    set_state(IMPROV_STATE_PROVISIONED);
    notify(s_h_rpc_result, s_rpc_result, s_rpc_result_len);
    ESP_LOGI(TAG, "provisioned; redirect %s", url);
    xSemaphoreGive(s_provisioned);
    vTaskDelete(NULL);
}

static void handle_rpc(const uint8_t *pkt, size_t len)
{
    if (len < 3 || pkt[1] != len - 3 ||
        checksum(pkt, len - 1) != pkt[len - 1]) {
        set_error(IMPROV_ERR_INVALID_RPC);
        return;
    }
    if (pkt[0] != IMPROV_CMD_WIFI_SETTINGS) {
        set_error(IMPROV_ERR_UNKNOWN_CMD);
        return;
    }
    const uint8_t *data = &pkt[2];
    size_t data_len = pkt[1];
    if (data_len < 2) {
        set_error(IMPROV_ERR_INVALID_RPC);
        return;
    }
    size_t ssid_len = data[0];
    if (1 + ssid_len + 1 > data_len ||
        ssid_len >= sizeof(s_pending_ssid)) {
        set_error(IMPROV_ERR_INVALID_RPC);
        return;
    }
    size_t pass_len = data[1 + ssid_len];
    if (1 + ssid_len + 1 + pass_len > data_len ||
        pass_len >= sizeof(s_pending_pass)) {
        set_error(IMPROV_ERR_INVALID_RPC);
        return;
    }
    memcpy(s_pending_ssid, &data[1], ssid_len);
    s_pending_ssid[ssid_len] = '\0';
    memcpy(s_pending_pass, &data[2 + ssid_len], pass_len);
    s_pending_pass[pass_len] = '\0';

    ESP_LOGI(TAG, "credentials received for \"%s\"", s_pending_ssid);
    set_error(IMPROV_ERR_NONE);
    set_state(IMPROV_STATE_PROVISIONING);
    xTaskCreate(provision_task, "improv_prov", 4096, NULL, 5, NULL);
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ble_uuid_cmp(uuid, &UUID_STATE.u) == 0) {
            return os_mbuf_append(ctxt->om, &s_state, 1);
        }
        if (ble_uuid_cmp(uuid, &UUID_ERROR.u) == 0) {
            return os_mbuf_append(ctxt->om, &s_error, 1);
        }
        if (ble_uuid_cmp(uuid, &UUID_CAPABILITIES.u) == 0) {
            uint8_t caps = 0x00; // no identify support
            return os_mbuf_append(ctxt->om, &caps, 1);
        }
        if (ble_uuid_cmp(uuid, &UUID_RPC_RESULT.u) == 0) {
            return os_mbuf_append(ctxt->om, s_rpc_result, s_rpc_result_len);
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(uuid, &UUID_RPC_CMD.u) == 0) {
        uint8_t buf[160];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        handle_rpc(buf, len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_STATE.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_h_state },
            { .uuid = &UUID_ERROR.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_h_error },
            { .uuid = &UUID_RPC_CMD.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_RPC_RESULT.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_h_rpc_result },
            { .uuid = &UUID_CAPABILITIES.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    // Late GAP callbacks (e.g. the phone disconnecting right after success)
    // must not re-arm advertising while the stack is being torn down.
    if (s_stopping) {
        return;
    }
    // Service data (16-bit UUID 0x4677): state, capabilities, 4 reserved.
    // This is what the Home Assistant companion app scans for.
    static uint8_t svc_data[8];
    svc_data[0] = 0x77;
    svc_data[1] = 0x46;
    svc_data[2] = s_state;
    svc_data[3] = 0x00; // capabilities
    memset(&svc_data[4], 0, 4);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.svc_data_uuid16 = svc_data;
    fields.svc_data_uuid16_len = sizeof(svc_data);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_device_name;
    rsp.name_len = strlen(s_device_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void improv_ble_start(const char *device_name, improv_connect_cb_t cb)
{
    s_device_name = device_name;
    s_connect_cb = cb;
    s_provisioned = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "heap before BLE: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(GATT_SVCS));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(GATT_SVCS));
    ble_svc_gap_device_name_set(device_name);
    nimble_port_freertos_init(host_task);
}

void improv_ble_wait_provisioned(void)
{
    xSemaphoreTake(s_provisioned, portMAX_DELAY);
}

void improv_ble_stop(void)
{
    s_stopping = true;
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    ESP_LOGI(TAG, "stopped");
}
