#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "fw_version.h"
#include "ota_update.h"
#include "panel_mqtt.h"
#include "ui_task.h"

static const char *TAG = "panel_mqtt";

#define TOPIC_LEN 96
#define PAYLOAD_MAX 12288 // the largest menu panel_core accepts is ~10 KB
#define STATUS_PERIOD_US (60 * 1000 * 1000LL)

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;

static char s_t_avail[TOPIC_LEN];
static char s_t_status[TOPIC_LEN];
static char s_t_request[TOPIC_LEN];
static char s_t_menu[TOPIC_LEN];
static char s_t_result[TOPIC_LEN];
static char s_t_ota[TOPIC_LEN];
static char s_t_state_prefix[TOPIC_LEN]; // "panel/<id>/state/"

// Reassembly of a fragmented incoming message (the topic is only present
// on the first fragment).
static char s_rx_topic[TOPIC_LEN + 64];
static char *s_rx_buf;
static bool s_rx_skip;

static void publish_status(void)
{
    if (!s_connected) {
        return;
    }
    int rssi = 0;
    esp_wifi_sta_get_rssi(&rssi);
    char body[64];
    snprintf(body, sizeof(body), "{\"version\":\"%s\",\"rssi\":%d}",
             PANEL_FW_VERSION, rssi);
    esp_mqtt_client_enqueue(s_client, s_t_status, body, 0, 1, true, true);
}

static void status_timer_cb(void *arg)
{
    publish_status();
}

static bool topic_is(const char *topic, const char *want)
{
    return strcmp(topic, want) == 0;
}

static void dispatch(const char *topic, char *payload)
{
    size_t prefix_len = strlen(s_t_state_prefix);
    if (topic_is(topic, s_t_menu)) {
        ui_task_menu(payload);
    } else if (topic_is(topic, s_t_result)) {
        ui_task_result(payload);
    } else if (strncmp(topic, s_t_state_prefix, prefix_len) == 0) {
        char *entity_id = strdup(topic + prefix_len);
        if (entity_id == NULL) {
            free(payload);
            return;
        }
        ui_task_state(entity_id, payload);
    } else if (topic_is(topic, s_t_ota)) {
        cJSON *root = cJSON_Parse(payload);
        const cJSON *url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            ota_update_start(url->valuestring);
        } else {
            ESP_LOGW(TAG, "OTA message without url");
        }
        cJSON_Delete(root);
        free(payload);
    } else {
        free(payload);
    }
}

static void on_data(esp_mqtt_event_handle_t evt)
{
    if (evt->current_data_offset == 0) {
        free(s_rx_buf);
        s_rx_buf = NULL;
        s_rx_skip = false;
        if (evt->topic_len >= (int)sizeof(s_rx_topic) ||
            evt->total_data_len > PAYLOAD_MAX) {
            ESP_LOGE(TAG, "dropping oversized message (%d bytes)",
                     evt->total_data_len);
            s_rx_skip = true;
            return;
        }
        memcpy(s_rx_topic, evt->topic, evt->topic_len);
        s_rx_topic[evt->topic_len] = '\0';
        s_rx_buf = malloc(evt->total_data_len + 1);
        if (s_rx_buf == NULL) {
            ESP_LOGE(TAG, "no memory for a %d byte message",
                     evt->total_data_len);
            s_rx_skip = true;
            return;
        }
    }
    if (s_rx_skip || s_rx_buf == NULL) {
        return;
    }
    memcpy(s_rx_buf + evt->current_data_offset, evt->data, evt->data_len);
    if (evt->current_data_offset + evt->data_len == evt->total_data_len) {
        s_rx_buf[evt->total_data_len] = '\0';
        char *payload = s_rx_buf;
        s_rx_buf = NULL; // ownership passes on
        dispatch(s_rx_topic, payload);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t event_id,
                          void *event_data)
{
    esp_mqtt_event_handle_t evt = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        esp_mqtt_client_enqueue(s_client, s_t_avail, "online", 0, 1, true,
                                true);
        publish_status();
        char filter[TOPIC_LEN + 2];
        snprintf(filter, sizeof(filter), "%s#", s_t_state_prefix);
        esp_mqtt_client_subscribe(s_client, s_t_menu, 1);
        esp_mqtt_client_subscribe(s_client, filter, 1);
        esp_mqtt_client_subscribe(s_client, s_t_result, 1);
        esp_mqtt_client_subscribe(s_client, s_t_ota, 1);
        // A healthy MQTT session is the post-OTA health check: only now
        // does a freshly flashed image escape bootloader rollback.
        esp_ota_mark_app_valid_cancel_rollback();
        ui_task_online(true);
        ui_task_status("");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ui_task_online(false);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error type=%d", evt->error_handle->error_type);
        break;
    case MQTT_EVENT_DATA:
        on_data(evt);
        break;
    default:
        break;
    }
}

void panel_mqtt_start(const device_cfg_t *cfg, const char *device_id)
{
    // device_id is at most DEVICE_ID_LEN - 1 (32) chars, so these fit.
    snprintf(s_t_avail, TOPIC_LEN, "panel/%s/availability", device_id);
    snprintf(s_t_status, TOPIC_LEN, "panel/%s/status", device_id);
    snprintf(s_t_request, TOPIC_LEN, "panel/%s/request", device_id);
    snprintf(s_t_menu, TOPIC_LEN, "panel/%s/menu", device_id);
    snprintf(s_t_result, TOPIC_LEN, "panel/%s/result", device_id);
    snprintf(s_t_ota, TOPIC_LEN, "panel/%s/ota", device_id);
    snprintf(s_t_state_prefix, TOPIC_LEN, "panel/%s/state/", device_id);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt_uri,
        .credentials = {
            .username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
            .authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass
                                                         : NULL,
        },
        .session.last_will = {
            .topic = s_t_avail,
            .msg = "offline",
            .qos = 1,
            .retain = true,
        },
        // Short keepalive: the broker notices a vanished panel (and HA
        // shows it offline) within ~half a minute.
        .session.keepalive = 15,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    const esp_timer_create_args_t timer_args = {
        .callback = status_timer_cb,
        .name = "mqtt_status",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, STATUS_PERIOD_US));
}

bool panel_mqtt_send_request(const request_t *req)
{
    if (!s_connected) {
        return false;
    }
    char body[160];
    if (request_to_json(req, body, sizeof(body)) < 0) {
        ESP_LOGE(TAG, "request too large to serialize");
        return false;
    }
    // Non-blocking: the MQTT task sends it. QoS 1 may deliver twice; HA
    // de-duplicates by request_id.
    int msg_id = esp_mqtt_client_enqueue(s_client, s_t_request, body, 0, 1,
                                         false, true);
    ESP_LOGI(TAG, "request %s: %s", msg_id >= 0 ? "queued" : "FAILED", body);
    return msg_id >= 0;
}
