/*
 * HUB75 LED matrix driver for ESP32-S3 (ESP-IDF v6).
 *
 * The panel is refreshed by the LCD_CAM peripheral's i80 bus: every HUB75
 * signal is one lane of the 16 bit parallel bus, CLK is the bus WR strobe,
 * and GDMA pushes the whole frame. A small task re-queues the frame forever
 * (LCD_CAM has no hardware loop mode), so the CPU cost is one interrupt per
 * frame regardless of what is on screen.
 *
 * The i80 driver requires a D/C pin, and with a 16 bit bus it requires all 16
 * data lanes to be real GPIOs even though HUB75 only uses 13. GPIO 15..18 are
 * assigned as dummies and should be left unconnected.
 *
 * Pin map (fixed at compile time, see hub75.c):
 *   Panel: 128x64, 1/32 scan.
 *   OE 21   CLK 47   LAT 14
 *   A 1   B 12   C 2   D 13   E 11
 *   R1 4   G1 9   B1 5   R2 6   G2 10   B2 7
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB75_WIDTH   128
#define HUB75_HEIGHT  64
#define HUB75_ROWS    (HUB75_HEIGHT / 2)   /* 1/32 scan: A..E address lines */
#define HUB75_PLANES  6                    /* binary code modulation depth  */

/* Pixel clock. A 128 wide panel needs twice the clocks per row, so this is
 * pushed up to keep the refresh rate sane. Drop to 8 MHz first if you see
 * ghosting or smeared columns; the ribbon cable is usually the limit. */
#define HUB75_PCLK_HZ (12 * 1000 * 1000)

/* Configures the i80 bus and allocates the DMA refresh buffer. Does not start
 * scanning yet -- call hub75_start(). */
esp_err_t hub75_init(void);

/* Starts / stops the refresh task that keeps the DMA fed. */
esp_err_t hub75_start(void);
esp_err_t hub75_stop(void);

/* Drawing. Colours are 8 bit per channel and are gamma corrected into the
 * HUB75_PLANES bit planes. Out-of-range coordinates are ignored. */
void hub75_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void hub75_fill(uint8_t r, uint8_t g, uint8_t b);
void hub75_clear(void);

/* Global brightness, 0..255. Implemented by shortening the OE windows, so it
 * costs no colour depth at the top end but crushes it at the bottom. */
void hub75_set_brightness(uint8_t brightness);
uint8_t hub75_get_brightness(void);

/* Measured refresh rate, in Hz, over the interval since the previous call.
 * Returns 0 on the first call. */
float hub75_refresh_hz(void);

/* Total frames pushed to the panel since hub75_start(). If this is not
 * climbing, the DMA is not running. */
uint32_t hub75_frame_count(void);

#ifdef __cplusplus
}
#endif
