#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Claude Code quota, polled from the ccusage_server that already runs for the
 * claude_usage_c3 trinket:
 *
 *   [~/.claude/.credentials.json] -> tools/ccusage_server.py :8787 /usage
 *                                    calls api.anthropic.com/api/oauth/usage
 *
 * The panel never talks to Anthropic itself -- it is just a second client of
 * that server, so the OAuth token stays on the PC. */

typedef struct {
    bool valid;            /* false until the first successful poll */
    bool stale;            /* server answered from cache, or we could not reach it */
    int  session_pct;      /* the five hour window */
    int  weekly_pct;       /* the seven day window */
    char session_resets[16];
    char weekly_resets[16];
    int64_t updated_us;
} usage_t;

/* Starts the background poller. Safe to call before Wi-Fi is up. */
void usage_start(void);

/* Copies the latest reading. Returns false if nothing has arrived yet. */
bool usage_get(usage_t *out);
