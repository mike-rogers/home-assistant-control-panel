#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

// Improv Wi-Fi provisioning over BLE (https://www.improv-wifi.com/ble/).
//
// Self-contained and project-agnostic: give it a callback that tries the
// credentials and returns success; the module handles the GATT service,
// advertising (with the Improv service-data the HA companion app scans
// for), RPC parsing, state/error reporting, and the redirect URL.
//
// Requires NimBLE (CONFIG_BT_NIMBLE_ENABLED) and, if Wi-Fi runs
// concurrently, software coexistence (default on).

// Called from a worker task with the received credentials. Return true
// once the network is joined (the module then reports PROVISIONED and a
// redirect URL derived from the device's IP), false to report
// "unable to connect" so the user can retry from the app.
typedef bool (*improv_connect_cb_t)(const char *ssid, const char *password);

// device_name appears in the phone's provisioning UI.
void improv_ble_start(const char *device_name, improv_connect_cb_t cb);

// Block until provisioning succeeded.
void improv_ble_wait_provisioned(void);

// Tear down advertising and the BLE stack (call after provisioning).
void improv_ble_stop(void);
