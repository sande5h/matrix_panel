#include <stdio.h>
#include <string.h>

#include "hub75.h"
#include "gfx.h"
#include "font5x7.h"


void gfx_char(int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
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

void gfx_marquee_at(int x0, int w, int y, const char *s, int scale, int *offset,
                    uint8_t r, uint8_t g, uint8_t b)
{
    int tw = gfx_text_width(s, scale);
    if (tw <= w) {
        gfx_text(x0 + (w - tw) / 2, y, s, scale, r, g, b);
        *offset = 0;
        return;
    }

    int span = tw + 8 * scale;          /* the gap between the two copies */
    int off = *offset % span;
    gfx_text(x0 - off, y, s, scale, r, g, b);
    gfx_text(x0 - off + span, y, s, scale, r, g, b);
    *offset = off + 1;
}

void gfx_marquee(int y, const char *s, int scale, int *offset,
                 uint8_t r, uint8_t g, uint8_t b)
{
    gfx_marquee_at(0, HUB75_WIDTH, y, s, scale, offset, r, g, b);
}

void gfx_blit_rgb565(int x, int y, int w, int h, const uint16_t *px)
{
    if (!px) return;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t p = px[row * w + col];
            /* 5/6/5 back out to 8 bits a channel, replicating the high bits so
             * full-scale stays full-scale rather than 248. */
            uint8_t r = (uint8_t)((p >> 11) & 0x1F);
            uint8_t g = (uint8_t)((p >> 5) & 0x3F);
            uint8_t b = (uint8_t)(p & 0x1F);
            hub75_set_pixel(x + col, y + row,
                            (uint8_t)((r << 3) | (r >> 2)),
                            (uint8_t)((g << 2) | (g >> 4)),
                            (uint8_t)((b << 3) | (b >> 2)));
        }
    }
}
