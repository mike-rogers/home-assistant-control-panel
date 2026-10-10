#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Menu tree as published by Home Assistant on panel/<id>/menu (spec §5).
//
// Items live in one flat array; every list (the root, or one submenu's
// children) occupies a contiguous run of it, so a list is just
// (first, count). Nothing is heap-allocated after parsing.

#define MENU_SCHEMA 1
#define MENU_MAX_ITEMS 64
#define MENU_MAX_LIST 20  // items in any one list
#define MENU_MAX_DEPTH 3  // root list = depth 1
#define MENU_ID_LEN 32
#define MENU_LABEL_LEN 48
#define MENU_ENTITY_LEN 64

typedef enum {
    MENU_ITEM_SUBMENU,
    MENU_ITEM_ACTION,
    MENU_ITEM_TIME,
    MENU_ITEM_INFO,
    MENU_ITEM_UNKNOWN, // newer firmware feature: shown greyed out
} menu_item_type_t;

typedef struct {
    char id[MENU_ID_LEN];
    char label[MENU_LABEL_LEN];
    char entity[MENU_ENTITY_LEN]; // time/info: the entity shown; else ""
    menu_item_type_t type;
    bool undo;
    bool confirm;
    uint16_t step_minutes;  // time items
    uint8_t first_child;    // submenus: index of the first child
    uint8_t child_count;
} menu_item_t;

typedef struct {
    char title[MENU_LABEL_LEN];
    uint8_t root_count; // the root list always starts at items[0]
    uint8_t count;
    menu_item_t items[MENU_MAX_ITEMS];
} menu_t;

typedef enum {
    MENU_OK,
    MENU_ERR_JSON,    // not parseable / not an object
    MENU_ERR_SCHEMA,  // a schema version this firmware doesn't understand
    MENU_ERR_INVALID, // structurally wrong or over a limit
} menu_status_t;

// Parse a menu payload. On anything but MENU_OK, *out is left untouched
// (callers keep showing their last good menu) and err holds a short,
// human-readable reason.
menu_status_t menu_parse(const char *json, menu_t *out, char *err,
                         size_t err_len);

// Find an item by id; returns its index or -1.
int menu_find(const menu_t *menu, const char *id);
