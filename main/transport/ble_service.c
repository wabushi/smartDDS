#include "ble_service.h"
#include "command_api.h"
#include "config/board_config.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"
#include "store/config/ble_store_config.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

/* Provided by the ESP-IDF NimBLE store integration. */
void ble_store_config_init(void);

static const char *TAG = "BLE";
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_command_handle;
static uint16_t s_state_handle;
static uint16_t s_response_handle;
static uint8_t s_addr_type;
static bool s_state_notify_enabled;
static bool s_response_notify_enabled;
static struct ble_npl_event s_state_notify_event;
static SemaphoreHandle_t s_response_mutex;
static char s_response_text[2048] = "{\"ok\":false,\"error\":\"no command response yet\"}";
static size_t s_response_len = sizeof("{\"ok\":false,\"error\":\"no command response yet\"}") - 1;

/* These callbacks run in the NimBLE host task. Keep the sizeable JSON
 * scratch buffers out of that task's stack; a command can trigger a state
 * notification immediately after it is applied. */
#define BLE_STATE_JSON_MAX 2048
static char s_state_json[BLE_STATE_JSON_MAX];
static char s_response_json[sizeof(s_response_text)];

#define BLE_COMMAND_MAX_LEN 512
#define BLE_COMMAND_QUEUE_LEN 4

typedef struct {
    char text[BLE_COMMAND_MAX_LEN];
} ble_command_t;

static QueueHandle_t s_command_queue;

/* UUID generation 4: changing these values avoids stale phone GATT caches. */
/* Version 2 UUIDs force Android Web Bluetooth to discard cached GATT
 * characteristics from the earlier development service. */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(0x6f,0x5e,0x4d,0x3c,0x2b,0x1a,0x7f,0x9e,0x44,0x4d,0x8e,0x6d,0x41,0x00,0x6a,0x8f);
static const ble_uuid128_t s_command_uuid = BLE_UUID128_INIT(0x6f,0x5e,0x4d,0x3c,0x2b,0x1a,0x7f,0x9e,0x44,0x4d,0x8e,0x6d,0x42,0x00,0x6a,0x8f);
static const ble_uuid128_t s_state_uuid = BLE_UUID128_INIT(0x6f,0x5e,0x4d,0x3c,0x2b,0x1a,0x7f,0x9e,0x44,0x4d,0x8e,0x6d,0x43,0x00,0x6a,0x8f);
static const ble_uuid128_t s_response_uuid = BLE_UUID128_INIT(0x6f,0x5e,0x4d,0x3c,0x2b,0x1a,0x7f,0x9e,0x44,0x4d,0x8e,0x6d,0x44,0x00,0x6a,0x8f);

static int gap_event(struct ble_gap_event *event, void *arg);

static void resolve_handles(void)
{
    int rc;

    rc = ble_gatts_find_chr(&s_service_uuid.u, &s_command_uuid.u,
                            NULL, &s_command_handle);
    ESP_LOGI(TAG, "command characteristic handle: rc=%d value=%u",
             rc, (unsigned)s_command_handle);
    rc = ble_gatts_find_chr(&s_service_uuid.u, &s_state_uuid.u,
                            NULL, &s_state_handle);
    ESP_LOGI(TAG, "state characteristic handle: rc=%d value=%u",
             rc, (unsigned)s_state_handle);
    rc = ble_gatts_find_chr(&s_service_uuid.u, &s_response_uuid.u,
                            NULL, &s_response_handle);
    ESP_LOGI(TAG, "response characteristic handle: rc=%d value=%u",
             rc, (unsigned)s_response_handle);
}

static int append_state(struct os_mbuf *om)
{
    int len = command_api_get_state_json(s_state_json, sizeof(s_state_json));
    return len < 0 || (size_t)len >= sizeof(s_state_json) || os_mbuf_append(om, s_state_json, (size_t)len) != 0
        ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int append_response(struct os_mbuf *om)
{
    size_t response_len;

    if (!s_response_mutex || xSemaphoreTake(s_response_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    response_len = s_response_len;
    memcpy(s_response_json, s_response_text, response_len);
    xSemaphoreGive(s_response_mutex);

    return os_mbuf_append(om, s_response_json, response_len) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "GATT command access: conn=%u attr=%u op=%d len=%u",
             (unsigned)conn, (unsigned)attr, (int)ctxt->op,
             (unsigned)OS_MBUF_PKTLEN(ctxt->om));
    /* NimBLE GATT callbacks use the GATT operation values here.  The
     * BLE_ATT_ACCESS_OP_* values are ATT-server values and do not match
     * ctxt->op (a write arrives as BLE_GATT_ACCESS_OP_WRITE_CHR == 1). */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return append_state(ctxt->om);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    ble_command_t command = { 0 };
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(command.text)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, len, command.text) != 0) return BLE_ATT_ERR_UNLIKELY;
    command.text[len] = '\0';
    /* The payload may contain a Wi-Fi password. Never print command bodies. */
    ESP_LOGI(TAG, "BLE write received (%u bytes)", (unsigned)len);

    /* Do not execute commands or send notifications from the GATT access
     * callback. Android Web Bluetooth is particularly sensitive to a second
     * GATT operation while this callback is still completing. */
    if (!s_command_queue || xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full; rejecting BLE command");
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    ESP_LOGI(TAG, "BLE command queued");
    return 0;
}

/* GATT notifications must be issued from the NimBLE host task context. */
static void state_notify_event_cb(struct ble_npl_event *event)
{
    (void)event;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    int rc;
    if (s_state_notify_enabled) {
        int len = command_api_get_state_json(s_state_json, sizeof(s_state_json));
        if (len > 0 && (size_t)len < sizeof(s_state_json)) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(s_state_json, (size_t)len);
            if (om) {
                rc = ble_gatts_notify_custom(conn, s_state_handle, om);
                if (rc != 0) ESP_LOGW(TAG, "state notification failed: %d", rc);
            }
        }
    }

    /* The command response is separate from state.  This gives Web Bluetooth
     * clients an explicit acknowledgement/error for every queued command. */
    size_t response_len;
    if (s_response_notify_enabled && s_response_mutex && xSemaphoreTake(s_response_mutex, 0) == pdTRUE) {
        response_len = s_response_len;
        memcpy(s_response_json, s_response_text, response_len);
        xSemaphoreGive(s_response_mutex);
        struct os_mbuf *response_mbuf = ble_hs_mbuf_from_flat(s_response_json, response_len);
        if (response_mbuf) {
            rc = ble_gatts_notify_custom(conn, s_response_handle, response_mbuf);
            if (rc != 0) ESP_LOGW(TAG, "response notification failed: %d", rc);
        }
    }
}

static void command_task(void *arg)
{
    (void)arg;
    ble_command_t command;
    char response[sizeof(s_response_text)];

    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;

        int response_len = command_api_execute_json(command.text, response, sizeof(response));
        ESP_LOGI(TAG, "BLE command processed (%s)",
                 strstr(response, "\"ok\":true") ? "accepted" : "rejected");
        if (response_len > 0 && (size_t)response_len < sizeof(s_response_text) && s_response_mutex &&
            xSemaphoreTake(s_response_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(s_response_text, response, (size_t)response_len);
            s_response_text[response_len] = '\0';
            s_response_len = (size_t)response_len;
            xSemaphoreGive(s_response_mutex);
        }
        /* Queue from the FreeRTOS command task; the callback itself runs in
         * NimBLE host context and performs both notifications safely. */
        ble_service_notify_state();
    }
}

static int state_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? append_state(ctxt->om) : BLE_ATT_ERR_UNLIKELY;
}

static int response_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR ? append_response(ctxt->om) : BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    { .type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&s_service_uuid.u,
      .characteristics=(struct ble_gatt_chr_def[]) {
          /* Advertise both modes. The web client prefers the no-response
           * method on Android, while generic BLE tools can use acknowledged
           * writes when needed. */
          { .uuid=&s_command_uuid.u,
            .access_cb=access_cb,
            .val_handle=&s_command_handle,
            .flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP },
          { .uuid=&s_state_uuid.u, .access_cb=state_access_cb, .val_handle=&s_state_handle, .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY },
          { .uuid=&s_response_uuid.u, .access_cb=response_access_cb, .val_handle=&s_response_handle, .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY },
          { 0 } } },
    { 0 }
};

static void advertise(void)
{
    struct ble_gap_adv_params params = { .conn_mode=BLE_GAP_CONN_MODE_UND, .disc_mode=BLE_GAP_DISC_MODE_GEN };
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_hs_adv_fields rsp_fields = { 0 };
    int rc;

    /* Put the service UUID in the advertisement.  The complete device name
     * goes in the scan response so both fit within BLE payload limits. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE advertisement fields: %d", rc);
        return;
    }

    rsp_fields.name = (uint8_t *)BLE_DEVICE_NAME;
    rsp_fields.name_len = strlen(BLE_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE scan response fields: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "failed to start BLE advertising: %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        ESP_LOGI(TAG, "connect event: status=%d conn=%u",
                 event->connect.status, (unsigned)event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_state_notify_enabled = false;
            s_response_notify_enabled = false;
            ESP_LOGI(TAG, "connected; command handle=%u state handle=%u response handle=%u",
                     (unsigned)s_command_handle, (unsigned)s_state_handle,
                     (unsigned)s_response_handle);
        } else {
            advertise();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "disconnect event: conn=%u reason=%d",
                 (unsigned)event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_state_notify_enabled = false;
        s_response_notify_enabled = false;
        advertise();
    } else if (event->type == BLE_GAP_EVENT_SUBSCRIBE) {
        ESP_LOGI(TAG, "subscribe event: conn=%u attr=%u notify=%u indicate=%u",
                 (unsigned)event->subscribe.conn_handle,
                 (unsigned)event->subscribe.attr_handle,
                 (unsigned)event->subscribe.cur_notify,
                 (unsigned)event->subscribe.cur_indicate);
        if (event->subscribe.attr_handle == s_state_handle) {
            s_state_notify_enabled = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_response_handle) {
            s_response_notify_enabled = event->subscribe.cur_notify != 0;
        }
    } else if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        ESP_LOGI(TAG, "link encryption status=%d", event->enc_change.status);
    }
    return 0;
}

static void on_sync(void)
{
    uint8_t addr[6] = { 0 };
    int is_nrpa = 0;
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer BLE address type: %d", rc);
        return;
    }
    rc = ble_hs_id_copy_addr(s_addr_type, addr, &is_nrpa);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to read BLE address: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE advertising address: %02X:%02X:%02X:%02X:%02X:%02X (type=%u)",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             is_nrpa ? 2U : (unsigned)s_addr_type);
    resolve_handles();
    advertise();
    ESP_LOGI(TAG,"advertising as %s",BLE_DEVICE_NAME);
}
static void on_reset(int reason) { ESP_LOGW(TAG,"host reset reason=%d",reason); }
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }

esp_err_t ble_service_start(void)
{
    s_command_queue = xQueueCreate(BLE_COMMAND_QUEUE_LEN, sizeof(ble_command_t));
    if (!s_command_queue) return ESP_ERR_NO_MEM;
    s_response_mutex = xSemaphoreCreateMutex();
    if (!s_response_mutex) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(command_task, "ble_cmd", 16384, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        vSemaphoreDelete(s_response_mutex);
        s_response_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err=nimble_port_init(); if (err != ESP_OK) return err;
    ble_npl_event_init(&s_state_notify_event, state_notify_event_cb, NULL);
    ble_store_config_init();
    ble_hs_cfg.sync_cb=on_sync; ble_hs_cfg.reset_cb=on_reset;
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_NO_IO; ble_hs_cfg.sm_bonding=1; ble_hs_cfg.sm_mitm=0; ble_hs_cfg.sm_sc=1;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to count DDS GATT services: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to register DDS GATT service: %d", rc);
        return ESP_FAIL;
    }
    char uuid_text[40];
    ESP_LOGI(TAG, "registered DDS GATT service %s",
             ble_uuid_to_str(&s_service_uuid.u, uuid_text));
    ESP_LOGI(TAG, "local GATT database:");
    ble_gatts_show_local();
    ESP_LOGI(TAG, "GATT database registered; characteristic handles resolve after BLE host sync");
    nimble_port_freertos_init(host_task); return ESP_OK;
}

void ble_service_notify_state(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (!ble_npl_event_is_queued(&s_state_notify_event)) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_state_notify_event);
    }
}
