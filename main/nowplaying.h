#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* What the Mac is playing, pushed here rather than polled: only the Mac knows,
 * and it already runs Hammerspoon, so it POSTs to /nowplaying whenever the
 * track or the transport state changes.
 *
 * Nothing is persisted. If the Mac sleeps the updates stop, and after
 * NOWPLAYING_STALE_S the panel treats it as nothing playing rather than
 * leaving a stale track on screen forever. */

#define NOWPLAYING_STALE_S 30

/* Album art, pushed separately as raw RGB565 and only when the track changes:
 * it is 4.6 KiB, far too much to resend with every metadata update. 48x48 is
 * what fits beside the text on a 128 wide panel. */
#define ART_W 48
#define ART_H 48
#define ART_BYTES (ART_W * ART_H * 2)

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

/* Stores one pushed thumbnail. len must be exactly ART_BYTES. */
bool nowplaying_set_art(const uint8_t *rgb565, size_t len);

/* The stored thumbnail, or NULL if none has been pushed for this track. */
const uint16_t *nowplaying_art(void);

/* Dropped when the track changes, so the previous cover never lingers beside
 * a new title. */
void nowplaying_clear_art(void);
