#pragma once

#include <stdbool.h>

/* What the panel is currently showing. The HTTP server sets it, the main loop
 * reads it every frame, so a screen change takes effect within one frame
 * without either side blocking the other. */
typedef enum {
    SCREEN_CLOCK = 0,   /* minute sweep, date, time, quota bars */
    SCREEN_CLAUDE,      /* both quota windows, with countdowns */
    SCREEN_VIDEO,       /* the clip in the video partition */
    SCREEN_COUNT
} screen_t;

screen_t    screen_get(void);
void        screen_set(screen_t s);
screen_t    screen_next(void);            /* advance and return the new one */
const char *screen_name(screen_t s);      /* "clock", "claude", "video" */
bool        screen_from_name(const char *name, screen_t *out);
