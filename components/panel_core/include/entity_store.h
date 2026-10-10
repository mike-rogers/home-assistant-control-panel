#pragma once

#include <stdbool.h>

// Latest known state of each entity the menu references, fed by the
// retained panel/<id>/state/<entity_id> topics. Not thread-safe: owned by
// the UI task.

#define ENTITY_STORE_MAX 32
#define ENTITY_ID_LEN 64
#define ENTITY_STATE_LEN 32
#define ENTITY_UNIT_LEN 12

typedef struct {
    char entity_id[ENTITY_ID_LEN];
    char state[ENTITY_STATE_LEN];
    char unit[ENTITY_UNIT_LEN]; // attributes.unit_of_measurement, or ""
} entity_state_t;

typedef struct {
    int count;
    entity_state_t entries[ENTITY_STORE_MAX];
} entity_store_t;

void entity_store_init(entity_store_t *store);

// Apply a state payload ({"state": ..., "attributes": {...}}). An empty
// payload (a cleared retained topic) forgets the entity. Returns false if
// the payload is malformed or the store is full.
bool entity_store_update(entity_store_t *store, const char *entity_id,
                         const char *json);

// NULL when unknown.
const entity_state_t *entity_store_get(const entity_store_t *store,
                                       const char *entity_id);
