#include <string.h>

#include <stdio.h>

#include "esp_mac.h"
#include "nvs.h"

#include "device_cfg.h"

#define NAMESPACE "ctlpanel"

bool device_cfg_load(device_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(cfg->mqtt_uri);
    bool ok = nvs_get_str(handle, "mqtt_uri", cfg->mqtt_uri, &size) == ESP_OK &&
              cfg->mqtt_uri[0] != '\0';
    size = sizeof(cfg->mqtt_user);
    if (nvs_get_str(handle, "mqtt_user", cfg->mqtt_user, &size) != ESP_OK) {
        cfg->mqtt_user[0] = '\0';
    }
    size = sizeof(cfg->mqtt_pass);
    if (nvs_get_str(handle, "mqtt_pass", cfg->mqtt_pass, &size) != ESP_OK) {
        cfg->mqtt_pass[0] = '\0';
    }
    size = sizeof(cfg->wifi_ssid);
    if (nvs_get_str(handle, "wifi_ssid", cfg->wifi_ssid, &size) != ESP_OK) {
        cfg->wifi_ssid[0] = '\0';
    }
    size = sizeof(cfg->wifi_pass);
    if (nvs_get_str(handle, "wifi_pass", cfg->wifi_pass, &size) != ESP_OK) {
        cfg->wifi_pass[0] = '\0';
    }
    nvs_close(handle);
    return ok;
}

esp_err_t device_cfg_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_str(handle, "wifi_ssid", ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, "wifi_pass", password ? password : "")) ==
            ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t device_cfg_save(const device_cfg_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_str(handle, "mqtt_uri", cfg->mqtt_uri)) == ESP_OK &&
        (err = nvs_set_str(handle, "mqtt_user", cfg->mqtt_user)) == ESP_OK &&
        (err = nvs_set_str(handle, "mqtt_pass", cfg->mqtt_pass)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void device_cfg_device_id(char *buf, size_t len)
{
    if (CONFIG_CTLPANEL_DEVICE_ID[0] != '\0') {
        strlcpy(buf, CONFIG_CTLPANEL_DEVICE_ID, len);
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "panel-%02x%02x%02x", mac[3], mac[4], mac[5]);
}
