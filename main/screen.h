#pragma once

#include <stdbool.h>

/* What the panel is currently showing. The HTTP server sets it, the main loop
 * reads it every frame, so a screen change takes effect within one frame
 * without either side blocking the other. */
typedef enum {
    SCREEN_CLOCK = 0,   /* minute sweep, date, time, quota bars */
    SCREEN_CLAUDE,      /* both quota windows, with countdowns */
    SCREEN_NOWPLAYING,  /* whatever the Mac pushed to /nowplaying */
    SCREEN_VIDEO,       /* the clip in the video partition */
    SCREEN_CYCLE_COUNT, /* everything above is in the toggle cycle */

    /* Entered only by an upload and left when it finishes, so it is not part
     * of the cycle and /screen?s= will not select it. */
    SCREEN_UPDATE = SCREEN_CYCLE_COUNT,
    SCREEN_COUNT
} screen_t;

screen_t    screen_get(void);
void        screen_set(screen_t s);
screen_t    screen_next(void);            /* advance through the cycle only */
const char *screen_name(screen_t s);      /* "clock", "claude", "nowplaying", "video", "update" */
void        screen_restore(screen_t s);   /* back to s, unless it is SCREEN_UPDATE */
bool        screen_from_name(const char *name, screen_t *out);
