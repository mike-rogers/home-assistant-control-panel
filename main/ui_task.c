#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "entity_store.h"
#include "menu.h"
#include "menu_cache.h"
#include "panel_mqtt.h"
#include "protocol.h"
#include "render.h"
#include "ui_task.h"

static const char *TAG = "ui";

#define TICK_MS 100 // timer resolution: toasts, Oops countdown, idle
#define QUEUE_LEN 24
#define STATUS_LEN 48

typedef enum {
    MSG_INPUT,
    MSG_ONLINE,
    MSG_MENU,
    MSG_STATE,
    MSG_RESULT,
    MSG_STATUS,
} msg_type_t;

typedef struct {
    msg_type_t type;
    union {
        input_event_t input;
        bool online;
        struct {
            char *json;
            char *entity_id;
        };
        char status[STATUS_LEN];
    };
} msg_t;

static QueueHandle_t s_queue;

// Owned by the UI task.
static ui_sm_t s_sm;
static entity_store_t s_store;
static ui_view_t s_view, s_last_view;
static char s_status[STATUS_LEN], s_last_status[STATUS_LEN];
static bool s_rendered;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post(msg_t *msg)
{
    if (s_queue == NULL || xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping message type %d", msg->type);
        if (msg->type == MSG_MENU || msg->type == MSG_STATE ||
            msg->type == MSG_RESULT) {
            free(msg->json);
            free(msg->entity_id);
        }
    }
}

void ui_task_input(input_event_t evt)
{
    msg_t msg = {.type = MSG_INPUT, .input = evt};
    post(&msg);
}

void ui_task_online(bool online)
{
    msg_t msg = {.type = MSG_ONLINE, .online = online};
    post(&msg);
}

void ui_task_menu(char *json)
{
    msg_t msg = {.type = MSG_MENU, .json = json};
    post(&msg);
}

void ui_task_state(char *entity_id, char *json)
{
    msg_t msg = {.type = MSG_STATE, .json = json, .entity_id = entity_id};
    post(&msg);
}

void ui_task_result(char *json)
{
    msg_t msg = {.type = MSG_RESULT, .json = json};
    post(&msg);
}

void ui_task_status(const char *text)
{
    msg_t msg = {.type = MSG_STATUS};
    strlcpy(msg.status, text, sizeof(msg.status));
    post(&msg);
}

// A menu from NVS or MQTT. Only MQTT menus are cached (from_cache = false).
static void apply_menu(const char *json, bool from_cache)
{
    // Transient: ui_sm keeps its own copy, and RAM is tight on the C3.
    menu_t *menu = malloc(sizeof(*menu));
    if (menu == NULL) {
        ESP_LOGE(TAG, "no memory to parse the menu");
        return;
    }
    char err[64];
    menu_status_t status = menu_parse(json, menu, err, sizeof(err));
    if (status != MENU_OK) {
        ESP_LOGW(TAG, "menu rejected: %s", err);
        if (!from_cache) {
            ui_sm_menu_rejected(&s_sm, err, now_ms());
        }
    } else {
        ESP_LOGI(TAG, "menu loaded (%u items)%s", menu->count,
                 from_cache ? " from NVS" : "");
        ui_sm_set_menu(&s_sm, menu, now_ms());
        if (!from_cache) {
            menu_cache_save(json);
        }
    }
    free(menu);
}

static void send_request(const request_t *req)
{
    if (panel_mqtt_send_request(req)) {
        return;
    }
    // Couldn't even enqueue: fail it now instead of waiting for a timeout.
    result_t res = {.ok = false};
    strlcpy(res.request_id, req->request_id, sizeof(res.request_id));
    strlcpy(res.message, "Could not send", sizeof(res.message));
    ui_sm_on_result(&s_sm, &res, now_ms());
}

static void handle(msg_t *msg)
{
    uint32_t now = now_ms();
    switch (msg->type) {
    case MSG_INPUT: {
        request_t req;
        if (ui_sm_input(&s_sm, msg->input, now, &req)) {
            send_request(&req);
        }
        break;
    }
    case MSG_ONLINE:
        ui_sm_set_online(&s_sm, msg->online, now);
        break;
    case MSG_MENU:
        // An empty retained payload means HA cleared the menu topic (the
        // panel was removed from HA): keep showing the last good menu.
        if (msg->json[0] != '\0') {
            apply_menu(msg->json, false);
        }
        free(msg->json);
        break;
    case MSG_STATE:
        if (!entity_store_update(&s_store, msg->entity_id, msg->json)) {
            ESP_LOGW(TAG, "state for %s not stored", msg->entity_id);
        }
        free(msg->json);
        free(msg->entity_id);
        break;
    case MSG_RESULT: {
        result_t res;
        if (result_parse(msg->json, &res)) {
            ui_sm_on_result(&s_sm, &res, now);
        } else {
            ESP_LOGW(TAG, "malformed result: %s", msg->json);
        }
        free(msg->json);
        break;
    }
    case MSG_STATUS:
        strlcpy(s_status, msg->status, sizeof(s_status));
        break;
    }
}

static void ui_task(void *arg)
{
    for (;;) {
        msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            handle(&msg);
            // Drain whatever else arrived so a burst renders once.
            while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
                handle(&msg);
            }
        }
        uint32_t now = now_ms();
        ui_sm_tick(&s_sm, now);
        ui_sm_view(&s_sm, now, &s_view);
        if (!s_rendered || memcmp(&s_view, &s_last_view, sizeof(s_view)) != 0 ||
            strcmp(s_status, s_last_status) != 0) {
            render_view(&s_view, s_status);
            s_last_view = s_view;
            strlcpy(s_last_status, s_status, sizeof(s_last_status));
            s_rendered = true;
        }
    }
}

void ui_task_start(void)
{
    ui_sm_config_t cfg = UI_SM_CONFIG_DEFAULT;
#if CONFIG_KPANEL_CLOCK_24H
    cfg.clock_24h = true;
#endif
    entity_store_init(&s_store);
    ui_sm_init(&s_sm, &cfg, &s_store, esp_random(), now_ms());

    char *cached = menu_cache_load();
    if (cached != NULL) {
        apply_menu(cached, true);
        free(cached);
    }

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    xTaskCreate(ui_task, "ui", 6144, NULL, 6, NULL);
}
