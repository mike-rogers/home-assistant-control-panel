#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bring up the Wi-Fi driver in station mode (no credentials yet).
void wifi_conn_init(void);

// Try to join the given network. Blocks until an IP is obtained (true) or
// the attempt fails/times out (false). On success, auto-reconnect stays
// enabled for the rest of runtime.
bool wifi_conn_try(const char *ssid, const char *password,
                   uint32_t timeout_ms);
