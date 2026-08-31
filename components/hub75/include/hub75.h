/*
 * HUB75 LED matrix driver for ESP32-S3 (ESP-IDF v6).
 *
 * The panel is refreshed entirely by the PARLIO TX peripheral running a
 * looped DMA transfer, so the CPU is only ever touched when a pixel changes.
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

/* Allocates the DMA refresh buffer and configures PARLIO. Does not start
 * scanning yet -- call hub75_start(). */
esp_err_t hub75_init(void);

/* Starts / stops the looped DMA transfer that refreshes the panel. */
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

/* Measured refresh rate of the whole panel, in Hz. */
float hub75_refresh_hz(void);

#ifdef __cplusplus
}
#endif
