#pragma once

#include <stdbool.h>

// Start the mDNS advertisement (_ctlpanel._tcp) and the local HTTP adoption
// endpoint:
//   GET  /api/info    -> {"id", "version", "provisioned"}
//   POST /api/config  -> {"mqtt_uri", "mqtt_username", "mqtt_password"}
// A successful config POST persists to NVS and reboots the device.
void adopt_server_start(const char *device_id, bool provisioned);
