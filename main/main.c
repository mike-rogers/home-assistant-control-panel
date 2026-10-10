#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "adopt_server.h"
#include "device_cfg.h"
#include "improv_ble.h"
#include "input.h"
#include "panel_mqtt.h"
#include "render.h"
#include "ui_task.h"
#include "wifi_conn.h"

static const char *TAG = "kpanel";

// Improv hands us candidate credentials; trial them and persist on success.
static bool improv_try_connect(const char *ssid, const char *password)
{
    ui_task_status("Trying Wi-Fi...");
    if (!wifi_conn_try(ssid, password, 15000)) {
        ui_task_status("Wi-Fi failed - try again from the app");
        return false;
    }
    if (device_cfg_save_wifi(ssid, password) != ESP_OK) {
        ESP_LOGE(TAG, "could not persist Wi-Fi credentials");
    }
    return true;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // The UI comes up first, with the menu cached in NVS: the panel is
    // navigable before (and without) the network.
    render_init();
    ui_task_start();
    input_init();

    static char device_id[DEVICE_ID_LEN];
    device_cfg_device_id(device_id, sizeof(device_id));
    static device_cfg_t cfg;
    bool provisioned = device_cfg_load(&cfg);
    ESP_LOGI(TAG, "device id %s, %s", device_id,
             provisioned ? "adopted" : "awaiting adoption");

    wifi_conn_init();

    // Wi-Fi credentials: NVS (Improv-provisioned) first, then the optional
    // menuconfig dev fallback, else BLE provisioning via the HA app.
    const char *ssid = cfg.wifi_ssid[0] ? cfg.wifi_ssid
                                        : CONFIG_KPANEL_WIFI_SSID;
    const char *pass = cfg.wifi_ssid[0] ? cfg.wifi_pass
                                        : CONFIG_KPANEL_WIFI_PASSWORD;
    if (ssid[0]) {
        ui_task_status("Connecting to Wi-Fi...");
        while (!wifi_conn_try(ssid, pass, 20000)) {
            ui_task_status("Wi-Fi retrying...");
        }
    } else {
        ui_task_status("Set up Wi-Fi with the Home Assistant app");
        improv_ble_start(device_id, improv_try_connect);
        improv_ble_wait_provisioned();
        improv_ble_stop();
    }

    adopt_server_start(device_id, provisioned);
    if (!provisioned) {
        // A successful adoption POST saves the config and reboots us.
        ui_task_status("Add Kitchen Panel in Home Assistant");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    ui_task_status("Connecting to Home Assistant...");
    panel_mqtt_start(&cfg, device_id);
}
