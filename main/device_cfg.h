#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Provisioned configuration persisted in NVS: broker settings pushed by the
// Home Assistant integration during adoption, Wi-Fi credentials from Improv
// BLE provisioning (menuconfig values remain a dev-only fallback).
typedef struct {
    char mqtt_uri[128];
    char mqtt_user[64];
    char mqtt_pass[64];
    char wifi_ssid[33];
    char wifi_pass[65];
} device_cfg_t;

// Load the stored config; returns false when the BROKER side is
// unprovisioned. Wi-Fi fields are filled (empty when absent).
bool device_cfg_load(device_cfg_t *cfg);

// Save broker settings (adoption). Leaves Wi-Fi keys untouched.
esp_err_t device_cfg_save(const device_cfg_t *cfg);

// Save Wi-Fi credentials (Improv provisioning).
esp_err_t device_cfg_save_wifi(const char *ssid, const char *password);

// The panel's id: names its MQTT topics (panel/<id>/...), mDNS host and HA
// config entry. menuconfig can override; otherwise "panel-" + MAC tail.
#define DEVICE_ID_LEN 33
void device_cfg_device_id(char *buf, size_t len);
