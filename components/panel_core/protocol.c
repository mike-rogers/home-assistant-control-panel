#include <string.h>

#include "cJSON.h"

#include "protocol.h"

static const char *op_name(request_op_t op)
{
    switch (op) {
    case OP_SET:
        return "set";
    case OP_UNDO:
        return "undo";
    case OP_RUN:
    default:
        return "run";
    }
}

int request_to_json(const request_t *req, char *buf, size_t len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "request_id", req->request_id);
    cJSON_AddStringToObject(root, "item_id", req->item_id);
    cJSON_AddStringToObject(root, "op", op_name(req->op));
    if (req->op == OP_SET) {
        cJSON_AddStringToObject(root, "value", req->value);
    } else {
        cJSON_AddNullToObject(root, "value");
    }
    bool ok = cJSON_PrintPreallocated(root, buf, (int)len, false);
    cJSON_Delete(root);
    return ok ? (int)strlen(buf) : -1;
}

bool result_parse(const char *json, result_t *out)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!cJSON_IsString(request_id) || !cJSON_IsString(status)) {
        cJSON_Delete(root);
        return false;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->request_id, request_id->valuestring,
            sizeof(out->request_id));
    out->ok = strcmp(status->valuestring, "ok") == 0;

    const cJSON *message = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(message)) {
        strlcpy(out->message, message->valuestring, sizeof(out->message));
    }
    const cJSON *undo = cJSON_GetObjectItem(root, "undo_seconds");
    if (cJSON_IsNumber(undo) && undo->valueint > 0) {
        out->undo_seconds = undo->valueint;
    }
    cJSON_Delete(root);
    return true;
}
