#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

#include "ota_update.h"
#include "ui_task.h"

static const char *TAG = "ota";

static char s_url[256];
static volatile bool s_in_progress;

static void ota_task(void *arg)
{
    ui_task_status("Updating firmware...");
    ESP_LOGI(TAG, "starting OTA from %s", s_url);

    esp_http_client_config_t http_cfg = {
        .url = s_url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update written, restarting");
        ui_task_status("Update installed - restarting");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    ui_task_status("Update failed");
    s_in_progress = false;
    vTaskDelete(NULL);
}

void ota_update_start(const char *url)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "update already in progress");
        return;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "refusing OTA from non-http url: %s", url);
        return;
    }
    if (strlen(url) >= sizeof(s_url)) {
        ESP_LOGE(TAG, "OTA url too long");
        return;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_in_progress = true;
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}
