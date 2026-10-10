#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "entity_store.h"
#include "menu.h"
#include "protocol.h"

// The panel's UI logic (spec §6) as a pure state machine: logical input,
// menu/result/connectivity updates and clock ticks go in; requests to
// publish come out, and ui_sm_view() describes what to draw. No I/O, no
// clock of its own (callers pass now_ms), no LVGL.

typedef enum {
    INPUT_NAV_NEXT,
    INPUT_NAV_PREV,
    INPUT_SELECT,
    INPUT_BACK,
    INPUT_HOME,
    INPUT_UNDO,
} input_event_t;

typedef struct {
    uint32_t toast_ms;           // result/error toast lifetime
    uint32_t request_timeout_ms; // no result by then = HA unreachable
    uint32_t home_after_ms;      // inactivity before returning to the root
    uint32_t dim_after_ms;       // inactivity before dimming
    bool clock_24h;
} ui_sm_config_t;

#define UI_SM_CONFIG_DEFAULT { \
    .toast_ms = 2000,           \
    .request_timeout_ms = 5000, \
    .home_after_ms = 30000,     \
    .dim_after_ms = 60000,      \
    .clock_24h = false,         \
}

typedef enum {
    UI_SCREEN_MENU,
    UI_SCREEN_EDIT_TIME,
    UI_SCREEN_CONFIRM,
    UI_SCREEN_MESSAGE, // no usable menu yet: view.message says why
} ui_screen_t;

#define UI_TOAST_LEN 72

typedef struct {
    ui_sm_config_t cfg;
    const entity_store_t *store;

    menu_t menu;
    bool has_menu;
    char menu_error[UI_TOAST_LEN];

    ui_screen_t screen;
    uint8_t depth;                     // 0 = root list
    uint8_t path[MENU_MAX_DEPTH];      // opened submenu at each depth
    uint8_t cursor[MENU_MAX_DEPTH];    // selected row at each depth

    int edit_item;
    int edit_minutes;
    int edit_original;

    int confirm_item;
    bool confirm_yes;

    bool pending;
    request_t pending_req;
    char pending_label[MENU_LABEL_LEN];
    uint32_t pending_since;

    bool oops_live;
    char oops_item_id[MENU_ID_LEN];
    char oops_label[MENU_LABEL_LEN];
    uint32_t oops_deadline;

    bool toast_live;
    char toast[UI_TOAST_LEN];
    uint32_t toast_until;

    bool online;
    bool dimmed;
    bool homed; // already returned home for the current idle stretch
    uint32_t last_input;

    uint32_t boot_id;
    uint32_t seq;
} ui_sm_t;

// boot_id distinguishes request ids across reboots (e.g. esp_random()).
void ui_sm_init(ui_sm_t *sm, const ui_sm_config_t *cfg,
                const entity_store_t *store, uint32_t boot_id,
                uint32_t now_ms);

// A logical input event. Returns true and fills *req when a request must
// be published.
bool ui_sm_input(ui_sm_t *sm, input_event_t evt, uint32_t now_ms,
                 request_t *req);

// A new valid menu. Keeps the user's place where the items still exist.
void ui_sm_set_menu(ui_sm_t *sm, const menu_t *menu, uint32_t now_ms);

// A menu payload was rejected; reason is shown, the current menu is kept.
void ui_sm_menu_rejected(ui_sm_t *sm, const char *reason, uint32_t now_ms);

// A result from HA. Results that don't match the pending request are
// ignored (late, or meant for a previous boot).
void ui_sm_on_result(ui_sm_t *sm, const result_t *res, uint32_t now_ms);

// MQTT session up/down.
void ui_sm_set_online(ui_sm_t *sm, bool online, uint32_t now_ms);

// Advance timers (toast, request timeout, Oops window, idle). Call at
// least a few times per second.
void ui_sm_tick(ui_sm_t *sm, uint32_t now_ms);

// ---- View model ----

typedef enum {
    UI_ROW_ITEM,
    UI_ROW_SUBMENU,
    UI_ROW_DISABLED, // unknown item type: greyed, not selectable
    UI_ROW_OOPS,
} ui_row_kind_t;

#define UI_VIEW_ROWS (MENU_MAX_LIST + 1) // + the Oops row
#define UI_VALUE_LEN 24

typedef struct {
    ui_row_kind_t kind;
    char label[MENU_LABEL_LEN + 12];
    char value[UI_VALUE_LEN]; // preview ("6:45 AM"), or Oops countdown
} ui_row_t;

typedef struct {
    ui_screen_t screen;
    char title[96]; // breadcrumb, e.g. "Kitchen > Wake-up"

    // UI_SCREEN_MENU
    int row_count;
    int selected;
    ui_row_t rows[UI_VIEW_ROWS];

    // UI_SCREEN_EDIT_TIME
    char edit_label[MENU_LABEL_LEN];
    char edit_value[UI_VALUE_LEN];
    const char *edit_hint;

    // UI_SCREEN_CONFIRM
    char question[MENU_LABEL_LEN + 4];
    bool confirm_yes;

    // UI_SCREEN_MESSAGE
    char message[UI_TOAST_LEN];

    // Overlays, any screen
    bool has_toast;
    char toast[UI_TOAST_LEN];
    bool online;
    bool pending;
    bool dimmed;
} ui_view_t;

void ui_sm_view(const ui_sm_t *sm, uint32_t now_ms, ui_view_t *view);

// ---- Helpers, exposed for tests ----

// "06:45:00" or "2026-10-11 06:45:00" -> minutes after midnight; -1 if
// unparseable.
int ui_parse_time(const char *state);

// Minutes after midnight -> "6:45 AM" or "06:45".
void ui_format_time(int minutes, bool clock_24h, char *buf, size_t len);
