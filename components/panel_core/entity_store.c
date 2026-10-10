#include <string.h>

#include "cJSON.h"

#include "entity_store.h"

void entity_store_init(entity_store_t *store)
{
    memset(store, 0, sizeof(*store));
}

static int find(const entity_store_t *store, const char *entity_id)
{
    for (int i = 0; i < store->count; i++) {
        if (strcmp(store->entries[i].entity_id, entity_id) == 0) {
            return i;
        }
    }
    return -1;
}

bool entity_store_update(entity_store_t *store, const char *entity_id,
                         const char *json)
{
    if (strlen(entity_id) >= ENTITY_ID_LEN) {
        return false;
    }
    int idx = find(store, entity_id);

    if (json == NULL || json[0] == '\0') {
        if (idx >= 0) {
            store->entries[idx] = store->entries[--store->count];
        }
        return true;
    }

    cJSON *root = cJSON_Parse(json);
    const cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state)) {
        cJSON_Delete(root);
        return false;
    }
    if (idx < 0) {
        if (store->count >= ENTITY_STORE_MAX) {
            cJSON_Delete(root);
            return false;
        }
        idx = store->count++;
    }

    entity_state_t *entry = &store->entries[idx];
    strlcpy(entry->entity_id, entity_id, sizeof(entry->entity_id));
    strlcpy(entry->state, state->valuestring, sizeof(entry->state));
    const cJSON *unit = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "attributes"), "unit_of_measurement");
    strlcpy(entry->unit, cJSON_IsString(unit) ? unit->valuestring : "",
            sizeof(entry->unit));
    cJSON_Delete(root);
    return true;
}

const entity_state_t *entity_store_get(const entity_store_t *store,
                                       const char *entity_id)
{
    int idx = find(store, entity_id);
    return idx >= 0 ? &store->entries[idx] : NULL;
}
