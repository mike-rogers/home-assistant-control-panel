#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "menu.h"

#define DEFAULT_STEP_MINUTES 15
#define MAX_STEP_MINUTES 720

typedef struct {
    menu_t *menu;
    char *err;
    size_t err_len;
} parse_ctx_t;

static bool fail(parse_ctx_t *ctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->err, ctx->err_len, fmt, args);
    va_end(args);
    return false;
}

static menu_item_type_t parse_type(const char *type)
{
    if (strcmp(type, "submenu") == 0) {
        return MENU_ITEM_SUBMENU;
    }
    if (strcmp(type, "action") == 0) {
        return MENU_ITEM_ACTION;
    }
    if (strcmp(type, "time") == 0) {
        return MENU_ITEM_TIME;
    }
    if (strcmp(type, "info") == 0) {
        return MENU_ITEM_INFO;
    }
    return MENU_ITEM_UNKNOWN;
}

static bool parse_item(parse_ctx_t *ctx, const cJSON *obj, menu_item_t *item)
{
    const cJSON *id = cJSON_GetObjectItem(obj, "id");
    const cJSON *label = cJSON_GetObjectItem(obj, "label");
    const cJSON *type = cJSON_GetObjectItem(obj, "type");

    if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
        return fail(ctx, "item without an id");
    }
    // Ids are keys in requests and results, so they must not be truncated.
    if (strlen(id->valuestring) >= sizeof(item->id)) {
        return fail(ctx, "item id too long: %.20s...", id->valuestring);
    }
    if (menu_find(ctx->menu, id->valuestring) >= 0) {
        return fail(ctx, "duplicate item id: %s", id->valuestring);
    }
    if (!cJSON_IsString(label) || !cJSON_IsString(type)) {
        return fail(ctx, "item %s needs a label and a type", id->valuestring);
    }

    strlcpy(item->id, id->valuestring, sizeof(item->id));
    strlcpy(item->label, label->valuestring, sizeof(item->label));
    item->type = parse_type(type->valuestring);

    item->undo = cJSON_IsTrue(cJSON_GetObjectItem(obj, "undo"));
    // Anything that can't be undone gets a confirmation step by default.
    const cJSON *confirm = cJSON_GetObjectItem(obj, "confirm");
    item->confirm = cJSON_IsBool(confirm) ? cJSON_IsTrue(confirm) : !item->undo;

    if (item->type == MENU_ITEM_TIME || item->type == MENU_ITEM_INFO) {
        const cJSON *entity = cJSON_GetObjectItem(obj, "entity");
        if (!cJSON_IsString(entity) || entity->valuestring[0] == '\0' ||
            strlen(entity->valuestring) >= sizeof(item->entity)) {
            return fail(ctx, "item %s needs a valid entity", item->id);
        }
        strlcpy(item->entity, entity->valuestring, sizeof(item->entity));
    }

    if (item->type == MENU_ITEM_TIME) {
        const cJSON *step = cJSON_GetObjectItem(obj, "step_minutes");
        int minutes = cJSON_IsNumber(step) ? step->valueint
                                           : DEFAULT_STEP_MINUTES;
        if (minutes < 1 || minutes > MAX_STEP_MINUTES) {
            return fail(ctx, "item %s: step_minutes out of range", item->id);
        }
        item->step_minutes = (uint16_t)minutes;
    }
    return true;
}

// Lay out one list contiguously, then recurse into its submenus, so every
// list ends up as a contiguous run of items.
static bool parse_list(parse_ctx_t *ctx, const cJSON *arr, int depth,
                       uint8_t *first, uint8_t *count)
{
    if (!cJSON_IsArray(arr)) {
        return fail(ctx, "items must be an array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n > MENU_MAX_LIST) {
        return fail(ctx, "more than %d items in one list", MENU_MAX_LIST);
    }
    menu_t *menu = ctx->menu;
    if (menu->count + n > MENU_MAX_ITEMS) {
        return fail(ctx, "more than %d items in total", MENU_MAX_ITEMS);
    }

    uint8_t start = menu->count;
    *first = start;
    *count = (uint8_t)n;

    int i = 0;
    const cJSON *obj;
    cJSON_ArrayForEach(obj, arr) {
        if (!cJSON_IsObject(obj)) {
            return fail(ctx, "menu entries must be objects");
        }
        // Count each item as it lands so duplicate-id checks see it.
        if (!parse_item(ctx, obj, &menu->items[start + i])) {
            return false;
        }
        menu->count++;
        i++;
    }

    i = 0;
    cJSON_ArrayForEach(obj, arr) {
        menu_item_t *item = &menu->items[start + i++];
        if (item->type != MENU_ITEM_SUBMENU) {
            continue;
        }
        if (depth >= MENU_MAX_DEPTH) {
            return fail(ctx, "submenu %s nested deeper than %d levels",
                        item->id, MENU_MAX_DEPTH);
        }
        if (!parse_list(ctx, cJSON_GetObjectItem(obj, "items"), depth + 1,
                        &item->first_child, &item->child_count)) {
            return false;
        }
    }
    return true;
}

menu_status_t menu_parse(const char *json, menu_t *out, char *err,
                         size_t err_len)
{
    char scratch[1];
    if (err == NULL || err_len == 0) {
        err = scratch;
        err_len = sizeof(scratch);
    }
    err[0] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, err_len, "menu is not a JSON object");
        return MENU_ERR_JSON;
    }

    const cJSON *schema = cJSON_GetObjectItem(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != MENU_SCHEMA) {
        snprintf(err, err_len, "menu schema %d not supported (need %d)",
                 cJSON_IsNumber(schema) ? schema->valueint : -1, MENU_SCHEMA);
        cJSON_Delete(root);
        return MENU_ERR_SCHEMA;
    }

    // Parse into scratch space so a bad menu never clobbers the good one.
    menu_t *menu = calloc(1, sizeof(*menu));
    if (menu == NULL) {
        cJSON_Delete(root);
        snprintf(err, err_len, "out of memory");
        return MENU_ERR_INVALID;
    }
    const cJSON *title = cJSON_GetObjectItem(root, "title");
    if (cJSON_IsString(title)) {
        strlcpy(menu->title, title->valuestring, sizeof(menu->title));
    }

    parse_ctx_t ctx = {.menu = menu, .err = err, .err_len = err_len};
    uint8_t first;
    bool ok = parse_list(&ctx, cJSON_GetObjectItem(root, "items"), 1, &first,
                         &menu->root_count);
    cJSON_Delete(root);

    if (ok) {
        *out = *menu;
    }
    free(menu);
    return ok ? MENU_OK : MENU_ERR_INVALID;
}

int menu_find(const menu_t *menu, const char *id)
{
    for (int i = 0; i < menu->count; i++) {
        if (strcmp(menu->items[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}
