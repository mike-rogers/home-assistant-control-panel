#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "menu_cache.h"

static const char *TAG = "menu_cache";

#define NAMESPACE "ctlpanel_menu"
#define KEY "menu"
#define MAX_BYTES 8192 // a dozen-item menu is ~2 KB; NVS is 24 KB in total

char *menu_cache_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return NULL;
    }
    size_t size = 0;
    char *json = NULL;
    if (nvs_get_blob(handle, KEY, NULL, &size) == ESP_OK && size > 0 &&
        size <= MAX_BYTES && (json = malloc(size + 1)) != NULL) {
        if (nvs_get_blob(handle, KEY, json, &size) == ESP_OK) {
            json[size] = '\0';
        } else {
            free(json);
            json = NULL;
        }
    }
    nvs_close(handle);
    return json;
}

void menu_cache_save(const char *json)
{
    size_t len = strlen(json);
    if (len > MAX_BYTES) {
        ESP_LOGW(TAG, "menu is %u bytes; too large to cache", (unsigned)len);
        return;
    }
    char *cached = menu_cache_load();
    bool same = cached != NULL && strcmp(cached, json) == 0;
    free(cached);
    if (same) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, KEY, json, len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not cache menu: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "menu cached (%u bytes)", (unsigned)len);
    }
}
