#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mdns.h"
#include "cJSON.h"

#include "adopt_server.h"
#include "device_cfg.h"
#include "fw_version.h"

static const char *TAG = "adopt";

static char s_device_id[DEVICE_ID_LEN];
static bool s_provisioned;

static void restart_task(void *arg)
{
    // Give the HTTP response time to flush before rebooting.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    char body[160];
    snprintf(body, sizeof(body),
             "{\"id\":\"%s\",\"version\":\"%s\",\"provisioned\":%s}",
             s_device_id, PANEL_FW_VERSION,
             s_provisioned ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[512];
    size_t len = req->content_len;
    if (len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    esp_err_t result = ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    cJSON *uri = cJSON_GetObjectItem(root, "mqtt_uri");
    cJSON *user = cJSON_GetObjectItem(root, "mqtt_username");
    cJSON *pass = cJSON_GetObjectItem(root, "mqtt_password");

    device_cfg_t cfg = {0};
    if (cJSON_IsString(uri) && uri->valuestring[0] != '\0' &&
        strlen(uri->valuestring) < sizeof(cfg.mqtt_uri) &&
        (!cJSON_IsString(user) ||
         strlen(user->valuestring) < sizeof(cfg.mqtt_user)) &&
        (!cJSON_IsString(pass) ||
         strlen(pass->valuestring) < sizeof(cfg.mqtt_pass))) {
        strlcpy(cfg.mqtt_uri, uri->valuestring, sizeof(cfg.mqtt_uri));
        if (cJSON_IsString(user)) {
            strlcpy(cfg.mqtt_user, user->valuestring, sizeof(cfg.mqtt_user));
        }
        if (cJSON_IsString(pass)) {
            strlcpy(cfg.mqtt_pass, pass->valuestring, sizeof(cfg.mqtt_pass));
        }
        result = device_cfg_save(&cfg);
    }
    cJSON_Delete(root);

    if (result != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid or unstorable config");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "adopted: broker config saved, rebooting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void adopt_server_start(const char *device_id, bool provisioned)
{
    s_provisioned = provisioned;
    strlcpy(s_device_id, device_id, sizeof(s_device_id));

    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));
    const httpd_uri_t info_uri = {
        .uri = "/api/info", .method = HTTP_GET, .handler = info_get_handler,
    };
    const httpd_uri_t config_uri = {
        .uri = "/api/config", .method = HTTP_POST,
        .handler = config_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &info_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_uri));

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(s_device_id));
    ESP_ERROR_CHECK(mdns_instance_name_set("Kitchen Panel"));
    ESP_ERROR_CHECK(mdns_service_add("Kitchen Panel", "_kpanel", "_tcp", 80, NULL, 0));
    mdns_service_txt_item_set("_kpanel", "_tcp", "id", s_device_id);
    mdns_service_txt_item_set("_kpanel", "_tcp", "version", PANEL_FW_VERSION);
    mdns_service_txt_item_set("_kpanel", "_tcp", "provisioned",
                              provisioned ? "1" : "0");

    ESP_LOGI(TAG, "adoption endpoint up as %s.local (%s)", s_device_id,
             provisioned ? "provisioned" : "awaiting adoption");
}
