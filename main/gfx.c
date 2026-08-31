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

void gfx_bar(int y, char label, int pct, bool dim)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* Green while there is room, amber as it tightens, red near the limit --
     * readable at a glance from across the room, which a number is not. */
    uint8_t r, g, b;
    if (pct < 60)      { r = 0;   g = 200; b = 60;  }
    else if (pct < 85) { r = 255; g = 170; b = 0;   }
    else               { r = 255; g = 40;  b = 40;  }
    if (dim) { r /= 3; g /= 3; b /= 3; }

    /* Inset from both edges: hard against the panel border the row reads as
     * an artefact rather than as content. */
    const int margin = 4;
    const int track_x = margin + FONT5X7_W + 3, track_h = 7;
    const int track_w = HUB75_WIDTH - track_x - margin - 24;

    gfx_char(margin, y, label, 1, 80, 80, 100);
    gfx_rect(track_x, y, track_w, track_h, 40, 40, 55);
    if (pct > 0) {
        int fill = (track_w - 2) * pct / 100;
        if (fill < 1) fill = 1;
        gfx_fill_rect(track_x + 1, y + 1, fill, track_h - 2, r, g, b);
    }

    char txt[8];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    gfx_text(HUB75_WIDTH - margin - gfx_text_width(txt, 1), y, txt, 1, r, g, b);
}

void gfx_seconds_sweep(int sec)
{
    if (sec < 0) sec = 0;
    if (sec > 59) sec = 59;
    /* +1 so the row is never empty at :00 and reaches the full width at :59. */
    int len = (sec + 1) * HUB75_WIDTH / 60;
    for (int x = 0; x < len; x++) hub75_set_pixel(x, 0, 0, 200, 80);
}
