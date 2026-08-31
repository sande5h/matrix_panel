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

/* Filled and outlined rectangles, clipped by hub75_set_pixel. */
void gfx_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void gfx_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

/* A labelled progress bar: one character label, an outlined track filled to
 * pct, and the percentage right aligned. Used for the Claude quota rows. */
void gfx_bar(int y, char label, int pct, bool dim);
