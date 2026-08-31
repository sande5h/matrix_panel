#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Brings up NVS, the event loop and Wi-Fi in station mode, and starts
 * connecting. Returns as soon as the attempt is under way -- it reconnects on
 * its own from then on, so a dropped AP heals without the caller's help. */
esp_err_t net_start(void);

bool net_connected(void);
bool net_wait_connected(int timeout_ms);

/* Dotted-quad address, or "0.0.0.0" before DHCP has finished. */
const char *net_ip(void);

/* Starts SNTP and waits for the first sync, then sets the timezone so
 * localtime() is right. Needs the network up first. */
esp_err_t net_sync_time(int timeout_ms);

/* True once the clock has been set from SNTP -- before that the time is 1970
 * and must not be shown. */
bool net_time_valid(void);
