#include <stdio.h>
#include <string.h>

#include "ui_sm.h"

#define MINUTES_PER_DAY (24 * 60)

// ASCII only: LVGL's built-in fonts have no typographic punctuation.
static const char *EDIT_HINT = "Turn to change, press to save, left to cancel";

// Wrap-safe "a is at or after b" for millisecond counters.
static bool reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void show_toast(ui_sm_t *sm, const char *text, uint32_t now_ms,
                       uint32_t duration_ms)
{
    strlcpy(sm->toast, text, sizeof(sm->toast));
    sm->toast_live = true;
    sm->toast_until = now_ms + duration_ms;
}

static void toast(ui_sm_t *sm, const char *text, uint32_t now_ms)
{
    show_toast(sm, text, now_ms, sm->cfg.toast_ms);
}

// ---- Time helpers ----

int ui_parse_time(const char *state)
{
    if (state == NULL) {
        return -1;
    }
    // A datetime ("YYYY-MM-DD HH:MM:SS") carries the time after the space.
    const char *space = strchr(state, ' ');
    if (space != NULL) {
        state = space + 1;
    }
    int h, m;
    if (sscanf(state, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 ||
        m > 59) {
        return -1;
    }
    return h * 60 + m;
}

void ui_format_time(int minutes, bool clock_24h, char *buf, size_t len)
{
    int h = minutes / 60;
    int m = minutes % 60;
    if (clock_24h) {
        snprintf(buf, len, "%02d:%02d", h, m);
    } else {
        snprintf(buf, len, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m,
                 h < 12 ? "AM" : "PM");
    }
}

// Step to the next/previous multiple of step, so an off-grid value (6:50
// with a 15-minute step) lands on the grid (7:00 / 6:45) on the first turn.
static int step_time(int minutes, int step, int direction)
{
    int next;
    if (direction > 0) {
        next = (minutes / step + 1) * step;
    } else {
        next = ((minutes + step - 1) / step - 1) * step;
    }
    return ((next % MINUTES_PER_DAY) + MINUTES_PER_DAY) % MINUTES_PER_DAY;
}

// ---- Navigation ----

static void list_at(const ui_sm_t *sm, int depth, int *first, int *count)
{
    if (depth == 0) {
        *first = 0;
        *count = sm->menu.root_count;
    } else {
        const menu_item_t *parent = &sm->menu.items[sm->path[depth - 1]];
        *first = parent->first_child;
        *count = parent->child_count;
    }
}

static void current_list(const ui_sm_t *sm, int *first, int *count)
{
    list_at(sm, sm->depth, first, count);
}

static int row_count(const ui_sm_t *sm)
{
    int first, count;
    current_list(sm, &first, &count);
    return count + (sm->oops_live ? 1 : 0);
}

static uint8_t *cursor(ui_sm_t *sm)
{
    return &sm->cursor[sm->depth];
}

static void clamp_cursor(ui_sm_t *sm)
{
    int rows = row_count(sm);
    if (*cursor(sm) >= rows) {
        *cursor(sm) = rows > 0 ? rows - 1 : 0;
    }
}

static void go_home(ui_sm_t *sm)
{
    sm->screen = sm->has_menu ? UI_SCREEN_MENU : UI_SCREEN_MESSAGE;
    sm->depth = 0;
    sm->cursor[0] = 0;
}

static void back_to_menu(ui_sm_t *sm)
{
    sm->screen = UI_SCREEN_MENU;
    clamp_cursor(sm);
}

static void clear_oops(ui_sm_t *sm)
{
    sm->oops_live = false;
    // Leaving the cursor on the vanished Oops row would point past the end.
    if (sm->has_menu) {
        clamp_cursor(sm);
    }
}

// ---- Requests ----

static bool issue(ui_sm_t *sm, request_op_t op, const char *item_id,
                  const char *label, const char *value, uint32_t now_ms,
                  request_t *req)
{
    if (!sm->online) {
        // Never queue: a late "Good night" is worse than a visible failure.
        toast(sm, "Offline - not sent", now_ms);
        return false;
    }
    if (sm->pending) {
        toast(sm, "Still waiting for Home Assistant", now_ms);
        return false;
    }

    memset(req, 0, sizeof(*req));
    snprintf(req->request_id, sizeof(req->request_id), "%08lx-%lu",
             (unsigned long)sm->boot_id, (unsigned long)++sm->seq);
    strlcpy(req->item_id, item_id, sizeof(req->item_id));
    req->op = op;
    if (value != NULL) {
        strlcpy(req->value, value, sizeof(req->value));
    }

    sm->pending = true;
    sm->pending_req = *req;
    sm->pending_since = now_ms;
    strlcpy(sm->pending_label, label, sizeof(sm->pending_label));
    return true;
}

static bool request_undo(ui_sm_t *sm, uint32_t now_ms, request_t *req)
{
    if (!sm->oops_live) {
        toast(sm, "Nothing to undo", now_ms);
        return false;
    }
    return issue(sm, OP_UNDO, sm->oops_item_id, sm->oops_label, NULL, now_ms,
                 req);
}

// ---- Screens ----

static bool select_item(ui_sm_t *sm, int idx, uint32_t now_ms,
                        request_t *req)
{
    const menu_item_t *item = &sm->menu.items[idx];
    switch (item->type) {
    case MENU_ITEM_SUBMENU:
        sm->path[sm->depth] = (uint8_t)idx;
        sm->depth++;
        *cursor(sm) = 0;
        return false;

    case MENU_ITEM_ACTION:
        if (item->confirm) {
            sm->screen = UI_SCREEN_CONFIRM;
            sm->confirm_item = idx;
            sm->confirm_yes = false; // a stray press does nothing
            return false;
        }
        return issue(sm, OP_RUN, item->id, item->label, NULL, now_ms, req);

    case MENU_ITEM_TIME: {
        const entity_state_t *state = entity_store_get(sm->store,
                                                       item->entity);
        int minutes = ui_parse_time(state ? state->state : NULL);
        if (minutes < 0) {
            toast(sm, "No value from Home Assistant yet", now_ms);
            return false;
        }
        sm->screen = UI_SCREEN_EDIT_TIME;
        sm->edit_item = idx;
        sm->edit_minutes = minutes;
        sm->edit_original = minutes;
        return false;
    }

    case MENU_ITEM_INFO:
        return false;

    case MENU_ITEM_UNKNOWN:
    default:
        toast(sm, "Needs newer panel firmware", now_ms);
        return false;
    }
}

static bool input_menu(ui_sm_t *sm, input_event_t evt, uint32_t now_ms,
                       request_t *req)
{
    int first, count;
    current_list(sm, &first, &count);
    int rows = row_count(sm);

    switch (evt) {
    case INPUT_NAV_NEXT:
        if (*cursor(sm) + 1 < rows) {
            (*cursor(sm))++;
        }
        return false;
    case INPUT_NAV_PREV:
        if (*cursor(sm) > 0) {
            (*cursor(sm))--;
        }
        return false;
    case INPUT_SELECT:
        if (*cursor(sm) >= count) {
            return request_undo(sm, now_ms, req); // the Oops row
        }
        return select_item(sm, first + *cursor(sm), now_ms, req);
    case INPUT_BACK:
        if (sm->depth > 0) {
            sm->depth--;
            clamp_cursor(sm); // its Oops row may have expired meanwhile
        }
        return false;
    default:
        return false;
    }
}

static bool input_edit(ui_sm_t *sm, input_event_t evt, uint32_t now_ms,
                       request_t *req)
{
    const menu_item_t *item = &sm->menu.items[sm->edit_item];
    switch (evt) {
    case INPUT_NAV_NEXT:
    case INPUT_NAV_PREV:
        sm->edit_minutes = step_time(sm->edit_minutes, item->step_minutes,
                                     evt == INPUT_NAV_NEXT ? 1 : -1);
        return false;
    case INPUT_SELECT: {
        back_to_menu(sm);
        if (sm->edit_minutes == sm->edit_original) {
            return false; // nothing changed: nothing to send or undo
        }
        char value[REQUEST_VALUE_LEN];
        // edit_minutes stays within a day; the casts tell the compiler so.
        snprintf(value, sizeof(value), "%02u:%02u",
                 (uint8_t)(sm->edit_minutes / 60),
                 (uint8_t)(sm->edit_minutes % 60));
        return issue(sm, OP_SET, item->id, item->label, value, now_ms, req);
    }
    case INPUT_BACK:
        back_to_menu(sm);
        return false;
    default:
        return false;
    }
}

static bool input_confirm(ui_sm_t *sm, input_event_t evt, uint32_t now_ms,
                          request_t *req)
{
    switch (evt) {
    case INPUT_NAV_NEXT:
        sm->confirm_yes = true;
        return false;
    case INPUT_NAV_PREV:
        sm->confirm_yes = false;
        return false;
    case INPUT_SELECT: {
        back_to_menu(sm);
        if (!sm->confirm_yes) {
            return false;
        }
        const menu_item_t *item = &sm->menu.items[sm->confirm_item];
        return issue(sm, OP_RUN, item->id, item->label, NULL, now_ms, req);
    }
    case INPUT_BACK:
        back_to_menu(sm);
        return false;
    default:
        return false;
    }
}

// ---- Public API ----

void ui_sm_init(ui_sm_t *sm, const ui_sm_config_t *cfg,
                const entity_store_t *store, uint32_t boot_id,
                uint32_t now_ms)
{
    memset(sm, 0, sizeof(*sm));
    sm->cfg = *cfg;
    sm->store = store;
    sm->boot_id = boot_id;
    sm->screen = UI_SCREEN_MESSAGE;
    sm->last_input = now_ms;
}

bool ui_sm_input(ui_sm_t *sm, input_event_t evt, uint32_t now_ms,
                 request_t *req)
{
    sm->last_input = now_ms;
    sm->homed = false;
    if (sm->dimmed) {
        // Waking the screen must not also act on what's now visible.
        sm->dimmed = false;
        return false;
    }

    if (evt == INPUT_HOME) {
        go_home(sm);
        return false;
    }
    if (!sm->has_menu) {
        return false;
    }
    if (evt == INPUT_UNDO) {
        return request_undo(sm, now_ms, req);
    }

    switch (sm->screen) {
    case UI_SCREEN_MENU:
        return input_menu(sm, evt, now_ms, req);
    case UI_SCREEN_EDIT_TIME:
        return input_edit(sm, evt, now_ms, req);
    case UI_SCREEN_CONFIRM:
        return input_confirm(sm, evt, now_ms, req);
    default:
        return false;
    }
}

void ui_sm_set_menu(ui_sm_t *sm, const menu_t *menu, uint32_t now_ms)
{
    (void)now_ms;
    sm->menu_error[0] = '\0';

    if (!sm->has_menu) {
        sm->menu = *menu;
        sm->has_menu = true;
        go_home(sm);
        return;
    }

    // Remember where we are by id, then swap menus and walk back down.
    char selected[MENU_MAX_DEPTH][MENU_ID_LEN] = {{0}};
    char opened[MENU_MAX_DEPTH][MENU_ID_LEN] = {{0}};
    uint8_t old_depth = sm->depth;
    for (int d = 0; d <= old_depth && d < MENU_MAX_DEPTH; d++) {
        if (d < old_depth) {
            strlcpy(opened[d], sm->menu.items[sm->path[d]].id, MENU_ID_LEN);
        }
        int first, count;
        list_at(sm, d, &first, &count);
        if (sm->cursor[d] < count) {
            strlcpy(selected[d], sm->menu.items[first + sm->cursor[d]].id,
                    MENU_ID_LEN);
        }
    }
    char edit_id[MENU_ID_LEN] = "";
    if (sm->screen == UI_SCREEN_EDIT_TIME) {
        strlcpy(edit_id, sm->menu.items[sm->edit_item].id, sizeof(edit_id));
    } else if (sm->screen == UI_SCREEN_CONFIRM) {
        strlcpy(edit_id, sm->menu.items[sm->confirm_item].id,
                sizeof(edit_id));
    }

    sm->menu = *menu;
    sm->depth = 0;
    for (int d = 0; d <= old_depth && d < MENU_MAX_DEPTH; d++) {
        int first, count;
        current_list(sm, &first, &count);
        sm->cursor[d] = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(sm->menu.items[first + i].id, selected[d]) == 0) {
                sm->cursor[d] = (uint8_t)i;
                break;
            }
        }
        if (d == old_depth) {
            break;
        }
        // Descend only if the opened submenu still exists in this list.
        int idx = menu_find(&sm->menu, opened[d]);
        if (idx < first || idx >= first + count ||
            sm->menu.items[idx].type != MENU_ITEM_SUBMENU) {
            break;
        }
        sm->path[d] = (uint8_t)idx;
        sm->depth = (uint8_t)(d + 1);
    }
    clamp_cursor(sm);

    if (sm->screen == UI_SCREEN_EDIT_TIME || sm->screen == UI_SCREEN_CONFIRM) {
        int idx = menu_find(&sm->menu, edit_id);
        ui_screen_t want = sm->screen;
        menu_item_type_t need = want == UI_SCREEN_EDIT_TIME ? MENU_ITEM_TIME
                                                            : MENU_ITEM_ACTION;
        if (sm->depth != old_depth || idx < 0 ||
            sm->menu.items[idx].type != need) {
            back_to_menu(sm);
        } else if (want == UI_SCREEN_EDIT_TIME) {
            sm->edit_item = idx;
        } else {
            sm->confirm_item = idx;
        }
    } else if (sm->screen == UI_SCREEN_MESSAGE) {
        sm->screen = UI_SCREEN_MENU;
    }
}

void ui_sm_menu_rejected(ui_sm_t *sm, const char *reason, uint32_t now_ms)
{
    snprintf(sm->menu_error, sizeof(sm->menu_error), "Menu rejected: %s",
             reason);
    if (sm->has_menu) {
        // Keep working with the last good menu; just say so for a while.
        show_toast(sm, sm->menu_error, now_ms, sm->cfg.toast_ms * 3);
    }
}

void ui_sm_on_result(ui_sm_t *sm, const result_t *res, uint32_t now_ms)
{
    if (!sm->pending ||
        strcmp(res->request_id, sm->pending_req.request_id) != 0) {
        return;
    }
    sm->pending = false;

    // HA keeps only one undo snapshot: any answered request supersedes the
    // previous Oops, and HA says whether this one is undoable (it may be
    // even on error, when an action ran partway).
    clear_oops(sm);
    if (sm->pending_req.op != OP_UNDO && res->undo_seconds > 0) {
        sm->oops_live = true;
        strlcpy(sm->oops_item_id, sm->pending_req.item_id,
                sizeof(sm->oops_item_id));
        strlcpy(sm->oops_label, sm->pending_label, sizeof(sm->oops_label));
        sm->oops_deadline = now_ms + (uint32_t)res->undo_seconds * 1000u;
    }

    if (res->message[0] != '\0') {
        toast(sm, res->message, now_ms);
    } else if (!res->ok) {
        toast(sm, "Home Assistant reported an error", now_ms);
    } else {
        toast(sm, sm->pending_req.op == OP_UNDO ? "Undone" : "Done", now_ms);
    }
}

void ui_sm_set_online(ui_sm_t *sm, bool online, uint32_t now_ms)
{
    if (sm->online && !online && sm->pending) {
        sm->pending = false;
        toast(sm, "Connection lost", now_ms);
    }
    sm->online = online;
}

void ui_sm_tick(ui_sm_t *sm, uint32_t now_ms)
{
    if (sm->toast_live && reached(now_ms, sm->toast_until)) {
        sm->toast_live = false;
    }
    if (sm->pending &&
        reached(now_ms, sm->pending_since + sm->cfg.request_timeout_ms)) {
        sm->pending = false;
        // Whether HA acted is unknown, so don't offer to undo the old one.
        clear_oops(sm);
        toast(sm, "No response from Home Assistant", now_ms);
    }
    if (sm->oops_live && reached(now_ms, sm->oops_deadline)) {
        clear_oops(sm);
    }
    if (!sm->homed &&
        reached(now_ms, sm->last_input + sm->cfg.home_after_ms)) {
        sm->homed = true;
        if (sm->has_menu) {
            go_home(sm);
        }
    }
    if (!sm->dimmed &&
        reached(now_ms, sm->last_input + sm->cfg.dim_after_ms)) {
        sm->dimmed = true;
    }
}

// ---- View ----

static void build_title(const ui_sm_t *sm, char *buf, size_t len)
{
    strlcpy(buf, sm->menu.title[0] ? sm->menu.title : "Menu", len);
    for (int d = 0; d < sm->depth; d++) {
        strlcat(buf, " > ", len);
        strlcat(buf, sm->menu.items[sm->path[d]].label, len);
    }
}

static void item_preview(const ui_sm_t *sm, const menu_item_t *item,
                         char *buf, size_t len)
{
    buf[0] = '\0';
    if (item->type != MENU_ITEM_TIME && item->type != MENU_ITEM_INFO) {
        return;
    }
    const entity_state_t *state = entity_store_get(sm->store, item->entity);
    if (state == NULL) {
        strlcpy(buf, "--", len);
        return;
    }
    if (item->type == MENU_ITEM_TIME) {
        int minutes = ui_parse_time(state->state);
        if (minutes < 0) {
            strlcpy(buf, "--", len);
        } else {
            ui_format_time(minutes, sm->cfg.clock_24h, buf, len);
        }
        return;
    }
    if (state->unit[0] != '\0') {
        snprintf(buf, len, "%s %s", state->state, state->unit);
    } else {
        strlcpy(buf, state->state, len);
    }
}

void ui_sm_view(const ui_sm_t *sm, uint32_t now_ms, ui_view_t *view)
{
    memset(view, 0, sizeof(*view));
    view->screen = sm->screen;
    view->online = sm->online;
    view->pending = sm->pending;
    view->dimmed = sm->dimmed;
    if (sm->toast_live) {
        view->has_toast = true;
        strlcpy(view->toast, sm->toast, sizeof(view->toast));
    }

    if (!sm->has_menu) {
        view->screen = UI_SCREEN_MESSAGE;
        strlcpy(view->title, "Kitchen Panel", sizeof(view->title));
        strlcpy(view->message,
                sm->menu_error[0] ? sm->menu_error
                                  : "Waiting for the menu from Home Assistant",
                sizeof(view->message));
        return;
    }

    build_title(sm, view->title, sizeof(view->title));

    switch (sm->screen) {
    case UI_SCREEN_EDIT_TIME: {
        const menu_item_t *item = &sm->menu.items[sm->edit_item];
        strlcpy(view->edit_label, item->label, sizeof(view->edit_label));
        ui_format_time(sm->edit_minutes, sm->cfg.clock_24h, view->edit_value,
                       sizeof(view->edit_value));
        view->edit_hint = EDIT_HINT;
        return;
    }
    case UI_SCREEN_CONFIRM:
        snprintf(view->question, sizeof(view->question), "%s?",
                 sm->menu.items[sm->confirm_item].label);
        view->confirm_yes = sm->confirm_yes;
        return;
    default:
        break;
    }

    int first, count;
    current_list(sm, &first, &count);
    for (int i = 0; i < count; i++) {
        const menu_item_t *item = &sm->menu.items[first + i];
        ui_row_t *row = &view->rows[view->row_count++];
        strlcpy(row->label, item->label, sizeof(row->label));
        row->kind = item->type == MENU_ITEM_SUBMENU ? UI_ROW_SUBMENU
                    : item->type == MENU_ITEM_UNKNOWN ? UI_ROW_DISABLED
                                                      : UI_ROW_ITEM;
        item_preview(sm, item, row->value, sizeof(row->value));
    }
    if (sm->oops_live) {
        ui_row_t *row = &view->rows[view->row_count++];
        row->kind = UI_ROW_OOPS;
        snprintf(row->label, sizeof(row->label), "Oops: undo %s",
                 sm->oops_label);
        uint32_t left_ms = reached(now_ms, sm->oops_deadline)
                               ? 0
                               : sm->oops_deadline - now_ms;
        snprintf(row->value, sizeof(row->value), "%lus",
                 (unsigned long)((left_ms + 999) / 1000));
    }
    view->selected = sm->cursor[sm->depth];
}
