#pragma once

#include <stdint.h>

/* Text on the panel, using the 5x7 font. `scale` is an integer pixel
 * multiplier: 1 is 5x7, 3 is 15x21. Characters are spaced one scaled pixel
 * apart. Lowercase is folded to uppercase automatically -- the font has no
 * lowercase, and track titles arrive mixed case -- and anything still outside
 * 0x20..0x5A renders blank. */
void gfx_char(int x, int y, char c, int scale, uint8_t r, uint8_t g, uint8_t b);
void gfx_text(int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b);

/* Width in pixels the string will occupy, for centring and right-alignment. */
int gfx_text_width(const char *s, int scale);

/* Draws centred on the panel's width. */
void gfx_text_center(int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b);

/* Filled and outlined rectangles, clipped by hub75_set_pixel. */
void gfx_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void gfx_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

/* A bare progress bar: no label, no track, no number -- just a filled run
 * across the panel's full width, 0 to 100%. Reads as a level from across the
 * room, which is all these rows need to do. */
void gfx_bar_full(int y, int height, int pct, bool dim);

/* A bar that draws its unfilled remainder in a dim grey. Floating away from
 * the panel edge there is no reference for where 100% would be, so without the
 * track you cannot tell 40% from 80% at a glance. */
void gfx_bar_track(int x, int y, int w, int height, int pct, bool dim);

/* The green-amber-red used for every quota reading, so the two screens can
 * never drift apart. */
void gfx_level_color(int pct, bool dim, uint8_t *r, uint8_t *g, uint8_t *b);

/* The top row as a minute sweep: a line growing left to right, full width at
 * the 59th second. Reads as motion from across the room, where a two digit
 * seconds counter does not. */
void gfx_seconds_sweep(int sec);

/* Draws s centred if it fits, and scrolls it if it does not. *offset is the
 * caller's scroll position and is advanced by one pixel per call, so the speed
 * follows the render tick. Two copies are drawn a gap apart, which is what
 * makes the wrap seamless rather than a jump back to the start. */
void gfx_marquee(int y, const char *s, int scale, int *offset,
                 uint8_t r, uint8_t g, uint8_t b);

/* The same, confined to a column starting at x0. Text that fits is centred in
 * the column; text that does not scrolls from x0 and is allowed to run off to
 * the left, where the caller paints the thumbnail over it afterwards -- so a
 * long title reads as sliding behind the artwork. */
void gfx_marquee_at(int x0, int w, int y, const char *s, int scale, int *offset,
                    uint8_t r, uint8_t g, uint8_t b);

/* Draws a packed RGB565 image at x,y. Used for the album thumbnail, which
 * arrives already scaled and converted on the Mac -- the panel has no business
 * decoding a JPEG for something this small. */
void gfx_blit_rgb565(int x, int y, int w, int h, const uint16_t *px);
