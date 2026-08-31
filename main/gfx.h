#pragma once

#include <stdint.h>

/* Text on the panel, using the 5x7 font. `scale` is an integer pixel
 * multiplier: 1 is 5x7, 3 is 15x21. Characters are spaced one scaled pixel
 * apart, and anything outside 0x20..0x5A (or lowercase) renders blank, so
 * pass uppercase. */
void gfx_char(int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b);
void gfx_text(int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b);

/* Width in pixels the string will occupy, for centring and right-alignment. */
int gfx_text_width(const char *s, int scale);

/* Draws centred on the panel's width. */
void gfx_text_center(int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b);
