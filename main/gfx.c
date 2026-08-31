#include <stdio.h>
#include <string.h>

#include "hub75.h"
#include "gfx.h"
#include "font5x7.h"


void gfx_char(int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) return;
    const uint8_t *glyph = font5x7[(int)c - FONT5X7_FIRST];

    for (int col = 0; col < FONT5X7_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT5X7_H; row++) {
            if (!(bits & (1u << row))) continue;
            /* One font pixel becomes a scale x scale block. */
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    hub75_set_pixel(x + col * scale + dx, y + row * scale + dy, r, g, b);
                }
            }
        }
    }
}

void gfx_text(int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    for (; *s; s++) {
        gfx_char(x, y, *s, scale, r, g, b);
        x += (FONT5X7_W + 1) * scale;
    }
}

int gfx_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * (FONT5X7_W + 1) * scale - scale : 0;   /* no trailing gap */
}

void gfx_text_center(int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    gfx_text((HUB75_WIDTH - gfx_text_width(s, scale)) / 2, y, s, scale, r, g, b);
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) hub75_set_pixel(x + dx, y + dy, r, g, b);
    }
}

void gfx_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int dx = 0; dx < w; dx++) {
        hub75_set_pixel(x + dx, y, r, g, b);
        hub75_set_pixel(x + dx, y + h - 1, r, g, b);
    }
    for (int dy = 0; dy < h; dy++) {
        hub75_set_pixel(x, y + dy, r, g, b);
        hub75_set_pixel(x + w - 1, y + dy, r, g, b);
    }
}

void gfx_level_color(int pct, bool dim, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Green while there is room, amber as it tightens, red near the limit --
     * readable at a glance, which a number is not. */
    if (pct < 60)      { *r = 0;   *g = 200; *b = 60;  }
    else if (pct < 85) { *r = 255; *g = 170; *b = 0;   }
    else               { *r = 255; *g = 40;  *b = 40;  }
    if (dim) { *r /= 3; *g /= 3; *b /= 3; }
}

void gfx_bar_track(int x, int y, int w, int height, int pct, bool dim)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    gfx_fill_rect(x, y, w, height, 18, 18, 26);      /* the empty remainder */

    uint8_t r, g, b;
    gfx_level_color(pct, dim, &r, &g, &b);
    int len = w * pct / 100;
    if (pct > 0 && len < 1) len = 1;                 /* 1% must still show */
    if (len > 0) gfx_fill_rect(x, y, len, height, r, g, b);
}

void gfx_bar_full(int y, int height, int pct, bool dim)
{
    if (pct <= 0) return;               /* nothing to show, so show nothing */
    if (pct > 100) pct = 100;

    uint8_t r, g, b;
    gfx_level_color(pct, dim, &r, &g, &b);

    int len = HUB75_WIDTH * pct / 100;
    if (len < 1) len = 1;               /* 1% must still be visible */
    gfx_fill_rect(0, y, len, height, r, g, b);
}

void gfx_seconds_sweep(int sec)
{
    if (sec < 0) sec = 0;
    if (sec > 59) sec = 59;
    /* +1 so the row is never empty at :00 and reaches the full width at :59. */
    int len = (sec + 1) * HUB75_WIDTH / 60;
    for (int x = 0; x < len; x++) hub75_set_pixel(x, 0, 0, 200, 80);
}
