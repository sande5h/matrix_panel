#pragma once

/* Copy to secrets.h and fill in. secrets.h is gitignored so credentials stay
 * out of the repository. */

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

/* The ccusage_server.py from the claude_usage_c3 project, running on the PC.
   It holds the OAuth token; the panel only ever sees percentages. Check this
   matches the subnet the panel lands on -- this Mac has two interfaces. */
#define USAGE_URL "http://192.168.1.x:8787/usage"
