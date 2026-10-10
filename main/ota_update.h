#pragma once

// Start a firmware update from the given http(s) URL in a background task.
// Shows progress on the status line; reboots into the new image on success.
// The bootloader rolls back if the new image never reaches a healthy MQTT
// connection (see esp_ota_mark_app_valid_cancel_rollback in panel_mqtt.c).
void ota_update_start(const char *url);
