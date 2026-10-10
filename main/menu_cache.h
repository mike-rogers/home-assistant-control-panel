#pragma once

// The last valid menu JSON, persisted in NVS so the panel is usable right
// after boot, before (or without) MQTT.

// Heap copy of the cached menu (caller frees), or NULL.
char *menu_cache_load(void);

// Store json if it differs from what's cached (saves flash wear on every
// reconnect, since the retained menu is redelivered each time).
void menu_cache_save(const char *json);
