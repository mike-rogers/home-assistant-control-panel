#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "menu.h"

// Wire formats for panel/<id>/request and panel/<id>/result (spec §4).

#define REQUEST_ID_LEN 24
#define REQUEST_VALUE_LEN 8 // "HH:MM"
#define RESULT_MESSAGE_LEN 64

typedef enum {
    OP_RUN,
    OP_SET,
    OP_UNDO,
} request_op_t;

typedef struct {
    char request_id[REQUEST_ID_LEN];
    char item_id[MENU_ID_LEN];
    request_op_t op;
    char value[REQUEST_VALUE_LEN]; // OP_SET only; "" otherwise
} request_t;

typedef struct {
    char request_id[REQUEST_ID_LEN];
    bool ok;
    char message[RESULT_MESSAGE_LEN];
    int undo_seconds; // 0 = not undoable
} result_t;

// Serialize a request. Returns the length written, or -1 if it didn't fit.
int request_to_json(const request_t *req, char *buf, size_t len);

// Parse a result payload; false if malformed.
bool result_parse(const char *json, result_t *out);
