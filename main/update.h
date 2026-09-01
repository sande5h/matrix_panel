#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/* Registers /ota, /video and /reboot on an already started server. Split out
 * of server.c because flashing has its own failure modes -- a half written
 * partition is worse than a rejected request -- and they are easier to reason
 * about in one place. */
void update_register(httpd_handle_t server);

/* Marks the running image good so the bootloader stops holding the previous
 * slot in reserve. Call it once the app has proved it works, which here means
 * wifi is up and the server is answering. */
void update_confirm(void);

/* What to draw while an upload is running, or for a few seconds after one
 * fails. Returns false when there is nothing to show. `what` is a short label
 * ("OTA", "VIDEO"), pct is 0..100, and `msg` is NULL while it is going well. */
bool update_progress(const char **what, int *pct, const char **msg);

/* The "update" object for /status: running slot, app version, clip header. */
int update_json(char *buf, size_t len);
