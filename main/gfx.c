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
