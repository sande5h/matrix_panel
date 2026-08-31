#pragma once

#include "esp_err.h"

/* Starts the control API on port 8088. Call once the network is up.
 *
 *   GET  /            a small status page with buttons
 *   GET  /status      {"screen":"clock","screens":[...],"ip":"...","up_s":123}
 *   GET|POST /toggle  advance to the next screen, returns the new state
 *   GET  /screen?s=clock|usage|video   select one directly
 *
 * Everything answers JSON except "/", so the same endpoints serve both a
 * browser and the menu bar script. */
esp_err_t server_start(void);
