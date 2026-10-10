#pragma once

#include <stdbool.h>

#include "device_cfg.h"
#include "protocol.h"

// MQTT side of the panel contract (spec §4): availability (LWT) and
// status out; menu, entity states, results and OTA requests in, forwarded
// to the UI task. cfg must outlive the client.
void panel_mqtt_start(const device_cfg_t *cfg, const char *device_id);

// Enqueue a request for the MQTT task to send. False if it couldn't be
// queued (not connected, or the outbox is full). Queued requests that
// can't be delivered within CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS are
// dropped, so a request never fires long after the user pressed.
bool panel_mqtt_send_request(const request_t *req);
