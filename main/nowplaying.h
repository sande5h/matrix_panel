#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What the Mac is playing, pushed here rather than polled: only the Mac knows,
 * and it already runs Hammerspoon, so it POSTs to /nowplaying whenever the
 * track or the transport state changes.
 *
 * Nothing is persisted. If the Mac sleeps the updates stop, and after
 * NOWPLAYING_STALE_S the panel treats it as nothing playing rather than
 * leaving a stale track on screen forever. */

#define NOWPLAYING_STALE_S 30

typedef struct {
    bool valid;
    bool playing;          /* false means paused, but still a current track */
    char title[64];
    char artist[64];
    int  position_s;
    int  duration_s;
    int64_t updated_us;
} nowplaying_t;

/* Feeds one JSON body from the HTTP handler. Returns false if it will not
 * parse. */
bool nowplaying_update(const char *json);

/* Copies the current track. Returns false when nothing is playing, which
 * includes the case of updates having stopped arriving. */
bool nowplaying_get(nowplaying_t *out);
